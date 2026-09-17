// SPDX-License-Identifier: MIT
#include "tsro/protocol.hpp"

#include <chrono>

namespace tsro::proto {
namespace {

struct TypeName {
    MessageType type;
    const char* name;
    bool server_to_client;
};

// Single source of truth for the wire names, so encode/decode/direction can never disagree.
constexpr TypeName kTypes[] = {
    {MessageType::Hello, "hello", true},
    {MessageType::StateSnapshot, "state_snapshot", true},
    {MessageType::ConnectionChanged, "connection_changed", true},
    {MessageType::ChannelChanged, "channel_changed", true},
    {MessageType::UserJoined, "user_joined", true},
    {MessageType::UserLeft, "user_left", true},
    {MessageType::UserUpdated, "user_updated", true},
    {MessageType::SpeakingChanged, "speaking_changed", true},
    {MessageType::MuteChanged, "mute_changed", true},
    {MessageType::CommanderChanged, "commander_changed", true},
    {MessageType::WhisperChanged, "whisper_changed", true},
    {MessageType::ChatMessageMsg, "chat_message", true},
    {MessageType::Heartbeat, "heartbeat", true},
    {MessageType::Error, "error", true},
    {MessageType::ClientHello, "client_hello", false},
    {MessageType::ConfigurationUpdated, "configuration_updated", false},
    {MessageType::RequestSnapshot, "request_snapshot", false},
    {MessageType::Ping, "ping", false},
};

void put_opt(json::Value& o, const char* k, const std::optional<bool>& v) {
    if (v.has_value()) o.set(k, json::Value(*v));
}
void read_opt(const json::Value& o, const char* k, std::optional<bool>& out) {
    const json::Value* v = o.find(k);
    if (v && v->is_bool()) out = v->as_bool();
    else out.reset();
}
void put_identity(json::Value& o, std::uint16_t cid, const std::string& uid) {
    o.set("client_id", json::Value(static_cast<long long>(cid)));
    o.set("unique_id", json::Value(uid));
}
bool read_identity(const json::Value& v, std::uint16_t& cid, std::string& uid) {
    const json::Value* u = v.find("unique_id");
    if (u == nullptr || !u->is_string() || u->as_string().empty()) return false;
    cid = static_cast<std::uint16_t>(v.get_int("client_id", 0) & 0xFFFF);
    uid = u->as_string();
    return true;
}

}  // namespace

const char* to_string(MessageType t) noexcept {
    for (const auto& e : kTypes) {
        if (e.type == t) return e.name;
    }
    return "unknown";
}

MessageType parse_message_type(std::string_view s) noexcept {
    for (const auto& e : kTypes) {
        if (s == e.name) return e.type;
    }
    return MessageType::Unknown;
}

bool is_server_to_client(MessageType t) noexcept {
    for (const auto& e : kTypes) {
        if (e.type == t) return e.server_to_client;
    }
    return false;
}

std::int64_t now_unix_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string Envelope::encode() const {
    json::Value o{json::Object{}};
    o.set("v", json::Value(static_cast<long long>(version)));
    o.set("seq", json::Value(seq));
    o.set("ts", json::Value(ts));
    o.set("type", json::Value(to_string(type)));
    if (!server_uid.empty()) o.set("server", json::Value(server_uid));
    if (!data.is_null()) o.set("data", data);
    return o.dump();
}

DecodeResult decode(std::string_view line) {
    DecodeResult r;
    if (line.size() > kMaxMessageBytes) {
        r.status = DecodeStatus::TooLarge;
        r.error = "message exceeds maximum size";
        return r;
    }
    json::Limits lim;
    lim.max_total_bytes = kMaxMessageBytes;
    const json::ParseResult p = json::parse(line, lim);
    if (!p.ok) {
        r.status = DecodeStatus::NotJson;
        r.error = p.error;
        return r;
    }
    if (!p.value.is_object()) {
        r.status = DecodeStatus::NotObject;
        r.error = "envelope is not an object";
        return r;
    }
    const json::Value* vv = p.value.find("v");
    const json::Value* tv = p.value.find("type");
    if (vv == nullptr || !vv->is_number() || tv == nullptr || !tv->is_string()) {
        r.status = DecodeStatus::MissingField;
        r.error = "envelope missing 'v' or 'type'";
        return r;
    }
    const long long version = vv->as_int();
    if (version < kProtocolMin || version > kProtocolMax) {
        r.status = DecodeStatus::BadVersion;
        r.error = "unsupported protocol version";
        r.envelope.version = static_cast<int>(version);
        return r;
    }
    const MessageType type = parse_message_type(tv->as_string());
    if (type == MessageType::Unknown) {
        // Forward compatibility: a newer peer may send types we do not know. Counted and
        // dropped by the caller, never dispatched, never fatal.
        r.status = DecodeStatus::UnknownType;
        r.error = "unknown message type";
        return r;
    }
    r.envelope.version = static_cast<int>(version);
    r.envelope.seq = p.value.get_int("seq", 0);
    r.envelope.ts = p.value.get_int("ts", 0);
    r.envelope.type = type;
    r.envelope.server_uid = p.value.get_string("server");
    if (const json::Value* d = p.value.find("data")) {
        if (d->is_object()) r.envelope.data = *d;
    }
    r.status = DecodeStatus::Ok;
    return r;
}

std::string make_line(MessageType type, std::int64_t seq, std::int64_t ts,
                      std::string_view server_uid, json::Value data) {
    Envelope e;
    e.version = kProtocolVersion;
    e.seq = seq;
    e.ts = ts;
    e.type = type;
    e.server_uid = std::string(server_uid);
    e.data = std::move(data);
    std::string s = e.encode();
    s.push_back('\n');
    return s;
}

// --- server -> client ------------------------------------------------------------------------

json::Value encode(const HelloPayload& p) {
    json::Value o{json::Object{}};
    o.set("protocol_min", json::Value(static_cast<long long>(p.protocol_min)));
    o.set("protocol_max", json::Value(static_cast<long long>(p.protocol_max)));
    o.set("plugin_version", json::Value(p.plugin_version));
    o.set("ts_client_version", json::Value(p.ts_client_version));
    o.set("plugin_api_version", json::Value(static_cast<long long>(p.plugin_api_version)));
    json::Array caps;
    caps.reserve(p.capabilities.size());
    for (const auto& c : p.capabilities) caps.push_back(json::Value(c));
    o.set("capabilities", json::Value(std::move(caps)));
    return o;
}

bool decode(const json::Value& v, HelloPayload& p) {
    if (!v.is_object()) return false;
    p.protocol_min = static_cast<int>(v.get_int("protocol_min", kProtocolMin));
    p.protocol_max = static_cast<int>(v.get_int("protocol_max", kProtocolMax));
    p.plugin_version = v.get_string("plugin_version");
    p.ts_client_version = v.get_string("ts_client_version");
    p.plugin_api_version = static_cast<int>(v.get_int("plugin_api_version", 0));
    p.capabilities.clear();
    if (const json::Value* c = v.find("capabilities")) {
        for (const auto& e : c->as_array()) {
            if (e.is_string()) p.capabilities.push_back(e.as_string());
        }
    }
    return true;
}

json::Value encode(const ConnectionChangedPayload& p) {
    json::Value o{json::Object{}};
    o.set("connection", json::Value(to_string(p.connection)));
    o.set("reason", json::Value(to_string(p.reason)));
    if (p.error_code != 0) o.set("error", json::Value(static_cast<long long>(p.error_code)));
    return o;
}

bool decode(const json::Value& v, ConnectionChangedPayload& p) {
    if (!v.is_object()) return false;
    if (!parse_connection_state(v.get_string("connection"), p.connection)) return false;
    parse_disconnect_reason(v.get_string("reason", "unknown"), p.reason);
    p.error_code = static_cast<unsigned>(v.get_int("error", 0));
    return true;
}

json::Value encode(const ChannelChangedPayload& p) {
    json::Value o{json::Object{}};
    if (p.from.has_value()) o.set("from", encode_channel(*p.from));
    o.set("to", encode_channel(p.to));
    o.set("user_count", json::Value(static_cast<long long>(p.user_count)));
    return o;
}

bool decode(const json::Value& v, ChannelChangedPayload& p) {
    if (!v.is_object()) return false;
    const json::Value* to = v.find("to");
    if (to == nullptr || !to->is_object()) return false;
    if (!decode_channel(*to, p.to)) return false;
    p.from.reset();
    if (const json::Value* f = v.find("from")) {
        ChannelState c;
        if (f->is_object() && decode_channel(*f, c)) p.from = c;
    }
    p.user_count = static_cast<int>(v.get_int("user_count", 0));
    return true;
}

json::Value encode(const UserJoinedPayload& p) {
    json::Value o{json::Object{}};
    o.set("user", encode_user(p.user));
    o.set("cause", json::Value(to_string(p.cause)));
    if (!p.from_channel_name.empty()) {
        o.set("from_channel_name", json::Value(p.from_channel_name));
    }
    return o;
}

bool decode(const json::Value& v, UserJoinedPayload& p) {
    if (!v.is_object()) return false;
    const json::Value* u = v.find("user");
    if (u == nullptr || !decode_user(*u, p.user)) return false;
    parse_join_cause(v.get_string("cause", "moved"), p.cause);
    p.from_channel_name = v.get_string("from_channel_name");
    return true;
}

json::Value encode(const UserLeftPayload& p) {
    json::Value o{json::Object{}};
    if (!p.to_channel_name.empty()) o.set("to_channel_name", json::Value(p.to_channel_name));
    o.set("user", encode_user(p.user));
    o.set("cause", json::Value(to_string(p.cause)));
    if (p.to_channel_id != 0)
        o.set("to_channel_id", json::Value(static_cast<long long>(p.to_channel_id)));
    return o;
}

bool decode(const json::Value& v, UserLeftPayload& p) {
    if (!v.is_object()) return false;
    const json::Value* u = v.find("user");
    if (u == nullptr || !decode_user(*u, p.user)) return false;
    parse_join_cause(v.get_string("cause", "moved"), p.cause);
    p.to_channel_id = static_cast<std::uint64_t>(v.get_int("to_channel_id", 0));
    p.to_channel_name = v.get_string("to_channel_name");
    return true;
}

json::Value encode(const UserUpdatedPayload& p) {
    json::Value o{json::Object{}};
    o.set("user", encode_user(p.user));
    json::Array ch;
    ch.reserve(p.changed.size());
    for (const auto& c : p.changed) ch.push_back(json::Value(c));
    o.set("changed", json::Value(std::move(ch)));
    return o;
}

bool decode(const json::Value& v, UserUpdatedPayload& p) {
    if (!v.is_object()) return false;
    const json::Value* u = v.find("user");
    if (u == nullptr || !decode_user(*u, p.user)) return false;
    p.changed.clear();
    if (const json::Value* c = v.find("changed")) {
        for (const auto& e : c->as_array()) {
            if (e.is_string()) p.changed.push_back(e.as_string());
        }
    }
    return true;
}

json::Value encode(const SpeakingChangedPayload& p) {
    json::Value o{json::Object{}};
    put_identity(o, p.client_id, p.unique_id);
    o.set("talking", json::Value(p.talking));
    if (p.whisper) o.set("whisper", json::Value(true));
    return o;
}

bool decode(const json::Value& v, SpeakingChangedPayload& p) {
    if (!v.is_object() || !read_identity(v, p.client_id, p.unique_id)) return false;
    p.talking = v.get_bool("talking");
    p.whisper = v.get_bool("whisper");
    return true;
}

json::Value encode(const MuteChangedPayload& p) {
    json::Value o{json::Object{}};
    put_identity(o, p.client_id, p.unique_id);
    put_opt(o, "input_muted", p.input_muted);
    put_opt(o, "output_muted", p.output_muted);
    put_opt(o, "input_hardware", p.input_hardware);
    put_opt(o, "output_hardware", p.output_hardware);
    put_opt(o, "input_deactivated", p.input_deactivated);
    return o;
}

bool decode(const json::Value& v, MuteChangedPayload& p) {
    if (!v.is_object() || !read_identity(v, p.client_id, p.unique_id)) return false;
    read_opt(v, "input_muted", p.input_muted);
    read_opt(v, "output_muted", p.output_muted);
    read_opt(v, "input_hardware", p.input_hardware);
    read_opt(v, "output_hardware", p.output_hardware);
    read_opt(v, "input_deactivated", p.input_deactivated);
    return true;
}

json::Value encode(const CommanderChangedPayload& p) {
    json::Value o{json::Object{}};
    put_identity(o, p.client_id, p.unique_id);
    o.set("channel_commander", json::Value(p.channel_commander));
    return o;
}

bool decode(const json::Value& v, CommanderChangedPayload& p) {
    if (!v.is_object() || !read_identity(v, p.client_id, p.unique_id)) return false;
    p.channel_commander = v.get_bool("channel_commander");
    return true;
}

json::Value encode(const WhisperChangedPayload& p) {
    json::Value o{json::Object{}};
    put_identity(o, p.client_id, p.unique_id);
    o.set("direction", json::Value(p.direction));
    o.set("active", json::Value(p.active));
    return o;
}

bool decode(const json::Value& v, WhisperChangedPayload& p) {
    if (!v.is_object() || !read_identity(v, p.client_id, p.unique_id)) return false;
    p.direction = v.get_string("direction", "incoming");
    // v1 defines only the incoming direction; anything else is a peer we do not understand.
    if (p.direction != "incoming") return false;
    p.active = v.get_bool("active");
    return true;
}

json::Value encode(const HeartbeatPayload& p) {
    json::Value o{json::Object{}};
    o.set("uptime_ms", json::Value(p.uptime_ms));
    o.set("connected_clients", json::Value(static_cast<long long>(p.connected_clients)));
    if (p.nonce.has_value()) o.set("nonce", json::Value(*p.nonce));
    return o;
}

bool decode(const json::Value& v, HeartbeatPayload& p) {
    if (!v.is_object()) return false;
    p.uptime_ms = v.get_int("uptime_ms", 0);
    p.connected_clients = static_cast<int>(v.get_int("connected_clients", 0));
    p.nonce.reset();
    if (const json::Value* n = v.find("nonce")) {
        if (n->is_number()) p.nonce = n->as_int();
    }
    return true;
}

json::Value encode(const ErrorPayload& p) {
    json::Value o{json::Object{}};
    o.set("code", json::Value(p.code));
    o.set("message", json::Value(p.message));
    o.set("fatal", json::Value(p.fatal));
    return o;
}

bool decode(const json::Value& v, ErrorPayload& p) {
    if (!v.is_object()) return false;
    p.code = v.get_string("code", "internal");
    p.message = v.get_string("message");
    p.fatal = v.get_bool("fatal");
    return true;
}

// --- client -> server ------------------------------------------------------------------------

json::Value encode(const ClientHelloPayload& p) {
    json::Value o{json::Object{}};
    o.set("protocol", json::Value(static_cast<long long>(p.protocol)));
    o.set("client", json::Value(p.client));
    o.set("client_version", json::Value(p.client_version));
    o.set("process", json::Value(p.process));
    o.set("pid", json::Value(p.pid));
    return o;
}

bool decode(const json::Value& v, ClientHelloPayload& p) {
    if (!v.is_object()) return false;
    p.protocol = static_cast<int>(v.get_int("protocol", 0));
    if (p.protocol <= 0) return false;
    p.client = json::truncate_utf8(v.get_string("client", "unknown"), 64);
    p.client_version = json::truncate_utf8(v.get_string("client_version"), 32);
    p.process = json::truncate_utf8(v.get_string("process"), 260);
    p.pid = v.get_int("pid", 0);
    return true;
}

json::Value encode(const ConfigurationUpdatedPayload& p) {
    json::Value chat{json::Object{}};
    chat.set("channel", json::Value(p.chat.channel));
    chat.set("server", json::Value(p.chat.server));
    chat.set("private", json::Value(p.chat.priv));
    json::Value o{json::Object{}};
    o.set("chat", std::move(chat));
    o.set("max_chat_length", json::Value(static_cast<long long>(p.max_chat_length)));
    o.set("want_speaking_events", json::Value(p.want_speaking_events));
    return o;
}

bool decode(const json::Value& v, ConfigurationUpdatedPayload& p) {
    if (!v.is_object()) return false;
    // Absent means "off": a client must opt in explicitly, so a truncated or partial
    // configuration can never turn private-message forwarding on.
    p.chat = ChatSubscription{};
    if (const json::Value* c = v.find("chat")) {
        p.chat.channel = c->get_bool("channel", false);
        p.chat.server = c->get_bool("server", false);
        p.chat.priv = c->get_bool("private", false);
    }
    const long long n = v.get_int("max_chat_length", static_cast<long long>(kMaxChatChars));
    p.max_chat_length = static_cast<int>(n < 1 ? 1 : (n > static_cast<long long>(kMaxChatChars)
                                                          ? static_cast<long long>(kMaxChatChars)
                                                          : n));
    p.want_speaking_events = v.get_bool("want_speaking_events", true);
    return true;
}

json::Value encode(const RequestSnapshotPayload& p) {
    json::Value o{json::Object{}};
    o.set("reason", json::Value(p.reason));
    return o;
}

bool decode(const json::Value& v, RequestSnapshotPayload& p) {
    if (!v.is_object()) return false;
    p.reason = json::truncate_utf8(v.get_string("reason", "manual"), 32);
    return true;
}

json::Value encode(const PingPayload& p) {
    json::Value o{json::Object{}};
    o.set("nonce", json::Value(p.nonce));
    return o;
}

bool decode(const json::Value& v, PingPayload& p) {
    if (!v.is_object()) return false;
    p.nonce = v.get_int("nonce", 0);
    return true;
}

}  // namespace tsro::proto
