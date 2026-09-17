// SPDX-License-Identifier: MIT
// The normalised data model: the single contract between the TeamSpeak side and the rendering
// side. Nothing here knows about TeamSpeak types, ReShade, ImGui or Win32.
//
// Optional<T> is used for every field the SDK may not be able to supply for a given client.
// "Absent" and "false" are different states: absent means the indicator must be hidden, not
// that it is off. Collapsing the two would make the overlay render confident wrong answers.
#ifndef TSRO_MODEL_HPP
#define TSRO_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tsro/json.hpp"

namespace tsro {

enum class ConnectionState { Disconnected, Connecting, Connected };

enum class DisconnectReason { Unknown, User, Timeout, Kicked, Banned, ServerShutdown, ConnectionLost };

enum class ChatCategory { Channel, Server, Private };

enum class JoinCause { Moved, Connected, Disconnected, Timeout, Kicked, Banned };

const char* to_string(ConnectionState) noexcept;
const char* to_string(DisconnectReason) noexcept;
const char* to_string(ChatCategory) noexcept;
const char* to_string(JoinCause) noexcept;
bool parse_connection_state(std::string_view, ConnectionState& out) noexcept;
bool parse_disconnect_reason(std::string_view, DisconnectReason& out) noexcept;
bool parse_chat_category(std::string_view, ChatCategory& out) noexcept;
bool parse_join_cause(std::string_view, JoinCause& out) noexcept;

struct ServerState {
    std::uint64_t handler_id = 0;
    std::string unique_id;   ///< virtualserver_unique_identifier; stable across sessions.
    std::string name;
    ConnectionState connection = ConnectionState::Disconnected;

    bool operator==(const ServerState& o) const;
    bool operator!=(const ServerState& o) const { return !(*this == o); }
};

struct ChannelState {
    std::uint64_t id = 0;
    std::uint64_t parent_id = 0;
    std::string name;
    std::string parent_name;
    std::string path;        ///< "Parent/Child"; display-only.
    std::string topic;

    bool valid() const noexcept { return id != 0; }
    bool operator==(const ChannelState& o) const;
    bool operator!=(const ChannelState& o) const { return !(*this == o); }
};

/// One TeamSpeak client in the local user's channel.
struct UserState {
    std::uint16_t client_id = 0;   ///< anyID. Session-scoped and recycled — never a config key.
    std::string unique_id;         ///< CLIENT_UNIQUE_IDENTIFIER. The identity key.
    std::string nickname;
    std::string display_name;      ///< getClientDisplayName(); falls back to nickname.

    bool talking = false;
    bool whispering_to_me = false;
    bool is_self = false;

    std::optional<bool> input_muted;
    std::optional<bool> output_muted;        ///< CLIENT_OUTPUTONLY_MUTED — independent of mic.
    std::optional<bool> input_hardware;
    std::optional<bool> output_hardware;
    std::optional<bool> input_deactivated;   ///< Own client only, per the SDK.
    std::optional<bool> away;
    std::optional<bool> recording;
    std::optional<bool> channel_commander;
    std::optional<bool> priority_speaker;
    std::optional<bool> is_talker;
    std::optional<bool> has_avatar;
    std::optional<bool> locally_muted;
    /// From the client's own Contacts list, read out of its settings.db -- the plugin API has
    /// no friend call at all. Absent when the list could not be read, which is different from
    /// "not a friend": the overlay must not colour a stranger green because a file was locked.
    std::optional<bool> is_friend;
    std::optional<bool> is_blocked;
    /// The name the user gave them in TeamSpeak's Contacts dialog, if any.
    std::string friend_nickname;
    std::optional<int> talk_power;
    std::string away_message;
    std::string country;

    /// Suppressed is derived, not reported: a client that is not a talker in a moderated channel
    /// cannot transmit. Unknown when is_talker is unknown.
    std::optional<bool> suppressed() const noexcept {
        if (!is_talker.has_value()) return std::nullopt;
        return !*is_talker;
    }

    bool operator==(const UserState& o) const;
    bool operator!=(const UserState& o) const { return !(*this == o); }
};

struct ChatMessage {
    std::uint64_t id = 0;
    ChatCategory category = ChatCategory::Channel;
    std::string sender_unique_id;
    std::string sender_name;
    std::string channel_name;
    std::string text;
    std::int64_t timestamp_ms = 0;
    bool outgoing = false;
};

/// Everything the renderer is allowed to know.
struct OverlayState {
    ServerState server;
    ChannelState channel;
    std::vector<UserState> users;
    std::string self_unique_id;
    std::int64_t last_event_ms = 0;
    /// True when no message has arrived within the configured staleness window. The renderer is
    /// required to stop presenting the user list as live once this is set.
    bool stale = false;
    /// False until a snapshot has ever been received on the current connection.
    bool synchronised = false;

    const UserState* find_user(std::uint16_t client_id) const noexcept;
    const UserState* find_user_by_uid(std::string_view unique_id) const noexcept;
    const UserState* self() const noexcept;
    std::size_t talking_count() const noexcept;
};

// --- JSON mapping. Encoders are total; decoders are tolerant and never throw. ---
json::Value encode_user(const UserState&);
json::Value encode_channel(const ChannelState&);
json::Value encode_server(const ServerState&);
json::Value encode_snapshot(const OverlayState&);
json::Value encode_chat(const ChatMessage&);

bool decode_user(const json::Value&, UserState& out);
bool decode_channel(const json::Value&, ChannelState& out);
bool decode_server(const json::Value&, ServerState& out);
bool decode_snapshot(const json::Value&, OverlayState& out);
bool decode_chat(const json::Value&, ChatMessage& out);

}  // namespace tsro

#endif  // TSRO_MODEL_HPP
