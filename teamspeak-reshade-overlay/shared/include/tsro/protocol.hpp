// SPDX-License-Identifier: MIT
// Versioned message envelope and the typed payloads carried in it. See docs/protocol.md.
#ifndef TSRO_PROTOCOL_HPP
#define TSRO_PROTOCOL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tsro/model.hpp"

namespace tsro::proto {

inline constexpr int kProtocolVersion = 1;
inline constexpr int kProtocolMin = 1;
inline constexpr int kProtocolMax = 1;
inline constexpr std::size_t kMaxMessageBytes = 65536;
inline constexpr std::size_t kMaxChatChars = 1024;
inline constexpr std::size_t kMaxUsersPerSnapshot = 512;
inline constexpr int kHeartbeatIntervalMs = 2000;

enum class MessageType {
    Unknown,
    // server -> client
    Hello,
    StateSnapshot,
    ConnectionChanged,
    ChannelChanged,
    UserJoined,
    UserLeft,
    UserUpdated,
    SpeakingChanged,
    MuteChanged,
    CommanderChanged,
    WhisperChanged,
    ChatMessageMsg,
    Heartbeat,
    Error,
    // client -> server
    ClientHello,
    ConfigurationUpdated,
    RequestSnapshot,
    Ping,
};

const char* to_string(MessageType) noexcept;
MessageType parse_message_type(std::string_view) noexcept;
bool is_server_to_client(MessageType) noexcept;

/// The envelope every message shares.
struct Envelope {
    int version = kProtocolVersion;
    std::int64_t seq = 0;
    std::int64_t ts = 0;
    MessageType type = MessageType::Unknown;
    std::string server_uid;   ///< empty when the message is connection-independent
    json::Value data;         ///< object; may be null/absent

    std::string encode() const;
};

enum class DecodeStatus {
    Ok,
    NotJson,
    NotObject,
    MissingField,
    BadVersion,
    UnknownType,
    TooLarge,
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::NotJson;
    Envelope envelope;
    std::string error;
    bool ok() const noexcept { return status == DecodeStatus::Ok; }
    /// Whether this failure means the peer is malformed enough to warrant closing the pipe, as
    /// opposed to one droppable message.
    bool fatal() const noexcept {
        return status == DecodeStatus::NotJson || status == DecodeStatus::TooLarge ||
               status == DecodeStatus::NotObject;
    }
};

DecodeResult decode(std::string_view line);

// --- Typed payload builders (server side) ---------------------------------------------------

struct HelloPayload {
    int protocol_min = kProtocolMin;
    int protocol_max = kProtocolMax;
    std::string plugin_version;
    std::string ts_client_version;
    int plugin_api_version = 0;
    std::vector<std::string> capabilities;
};

struct ConnectionChangedPayload {
    ConnectionState connection = ConnectionState::Disconnected;
    DisconnectReason reason = DisconnectReason::Unknown;
    unsigned error_code = 0;
};

struct ChannelChangedPayload {
    std::optional<ChannelState> from;
    ChannelState to;
    int user_count = 0;
};

struct UserJoinedPayload {
    UserState user;
    JoinCause cause = JoinCause::Moved;
    /// The channel they came from, when we could see it. Empty when they joined the server
    /// outright or arrived from a channel we are not subscribed to.
    std::string from_channel_name;
};

struct UserLeftPayload {
    UserState user;
    JoinCause cause = JoinCause::Moved;
    std::uint64_t to_channel_id = 0;
    /// Where they went, when we could see it. Empty when they left the server or moved to a
    /// channel we are not subscribed to.
    std::string to_channel_name;
};

struct UserUpdatedPayload {
    UserState user;
    std::vector<std::string> changed;
};

struct SpeakingChangedPayload {
    std::uint16_t client_id = 0;
    std::string unique_id;
    bool talking = false;
    bool whisper = false;
};

struct MuteChangedPayload {
    std::uint16_t client_id = 0;
    std::string unique_id;
    std::optional<bool> input_muted;
    std::optional<bool> output_muted;
    std::optional<bool> input_hardware;
    std::optional<bool> output_hardware;
    std::optional<bool> input_deactivated;
};

struct CommanderChangedPayload {
    std::uint16_t client_id = 0;
    std::string unique_id;
    bool channel_commander = false;
};

struct WhisperChangedPayload {
    std::uint16_t client_id = 0;
    std::string unique_id;
    bool active = false;
    /// Always "incoming" in v1: no TeamSpeak plugin callback reports outgoing whisper state.
    std::string direction = "incoming";
};

struct HeartbeatPayload {
    std::int64_t uptime_ms = 0;
    int connected_clients = 0;
    std::optional<std::int64_t> nonce;
};

struct ErrorPayload {
    std::string code;
    std::string message;
    bool fatal = false;
};

json::Value encode(const HelloPayload&);
json::Value encode(const ConnectionChangedPayload&);
json::Value encode(const ChannelChangedPayload&);
json::Value encode(const UserJoinedPayload&);
json::Value encode(const UserLeftPayload&);
json::Value encode(const UserUpdatedPayload&);
json::Value encode(const SpeakingChangedPayload&);
json::Value encode(const MuteChangedPayload&);
json::Value encode(const CommanderChangedPayload&);
json::Value encode(const WhisperChangedPayload&);
json::Value encode(const HeartbeatPayload&);
json::Value encode(const ErrorPayload&);

bool decode(const json::Value&, HelloPayload&);
bool decode(const json::Value&, ConnectionChangedPayload&);
bool decode(const json::Value&, ChannelChangedPayload&);
bool decode(const json::Value&, UserJoinedPayload&);
bool decode(const json::Value&, UserLeftPayload&);
bool decode(const json::Value&, UserUpdatedPayload&);
bool decode(const json::Value&, SpeakingChangedPayload&);
bool decode(const json::Value&, MuteChangedPayload&);
bool decode(const json::Value&, CommanderChangedPayload&);
bool decode(const json::Value&, WhisperChangedPayload&);
bool decode(const json::Value&, HeartbeatPayload&);
bool decode(const json::Value&, ErrorPayload&);

// --- Client -> server payloads ---------------------------------------------------------------

struct ClientHelloPayload {
    int protocol = kProtocolVersion;
    std::string client = "reshade-addon";
    std::string client_version;
    std::string process;
    std::int64_t pid = 0;
};

/// Chat categories default to false on both sides. The plugin sends no chat at all until a
/// client explicitly asks, so a client that never sends this cannot receive private messages.
struct ChatSubscription {
    bool channel = false;
    bool server = false;
    bool priv = false;
    bool enabled_for(ChatCategory c) const noexcept {
        switch (c) {
            case ChatCategory::Channel: return channel;
            case ChatCategory::Server: return server;
            // A poke is as personal as a private message and rides the same switch, so
            // turning private messages on is all it takes to start receiving them.
            case ChatCategory::Private:
            case ChatCategory::Poke:
                return priv;
        }
        return false;
    }
};

struct ConfigurationUpdatedPayload {
    ChatSubscription chat;
    int max_chat_length = static_cast<int>(kMaxChatChars);
    bool want_speaking_events = true;
};

struct RequestSnapshotPayload {
    std::string reason = "manual";
};

struct PingPayload {
    std::int64_t nonce = 0;
};

json::Value encode(const ClientHelloPayload&);
json::Value encode(const ConfigurationUpdatedPayload&);
json::Value encode(const RequestSnapshotPayload&);
json::Value encode(const PingPayload&);

bool decode(const json::Value&, ClientHelloPayload&);
bool decode(const json::Value&, ConfigurationUpdatedPayload&);
bool decode(const json::Value&, RequestSnapshotPayload&);
bool decode(const json::Value&, PingPayload&);

/// Builds a wire line (envelope + '\n'). `seq` and `ts` are supplied by the caller's connection.
std::string make_line(MessageType type, std::int64_t seq, std::int64_t ts,
                      std::string_view server_uid, json::Value data);

/// Milliseconds since the Unix epoch from the system clock.
std::int64_t now_unix_ms() noexcept;

}  // namespace tsro::proto

#endif  // TSRO_PROTOCOL_HPP
