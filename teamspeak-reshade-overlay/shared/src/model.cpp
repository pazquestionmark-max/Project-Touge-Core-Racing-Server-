// SPDX-License-Identifier: MIT
#include "tsro/model.hpp"

#include <algorithm>

namespace tsro {
namespace {

/// Writes an optional<bool> only when engaged. Omission is meaningful (see model.hpp).
void put_opt(json::Value& o, const char* key, const std::optional<bool>& v) {
    if (v.has_value()) o.set(key, json::Value(*v));
}
void put_opt_int(json::Value& o, const char* key, const std::optional<int>& v) {
    if (v.has_value()) o.set(key, json::Value(static_cast<long long>(*v)));
}
void read_opt(const json::Value& o, const char* key, std::optional<bool>& out) {
    const json::Value* v = o.find(key);
    if (v && v->is_bool()) out = v->as_bool();
    else out.reset();
}
void read_opt_int(const json::Value& o, const char* key, std::optional<int>& out) {
    const json::Value* v = o.find(key);
    if (v && v->is_number()) out = static_cast<int>(v->as_int());
    else out.reset();
}
void put_str(json::Value& o, const char* key, const std::string& s) {
    if (!s.empty()) o.set(key, json::Value(s));
}

}  // namespace

const char* to_string(ConnectionState s) noexcept {
    switch (s) {
        case ConnectionState::Disconnected: return "disconnected";
        case ConnectionState::Connecting: return "connecting";
        case ConnectionState::Connected: return "connected";
    }
    return "disconnected";
}

const char* to_string(DisconnectReason r) noexcept {
    switch (r) {
        case DisconnectReason::User: return "user";
        case DisconnectReason::Timeout: return "timeout";
        case DisconnectReason::Kicked: return "kicked";
        case DisconnectReason::Banned: return "banned";
        case DisconnectReason::ServerShutdown: return "server_shutdown";
        case DisconnectReason::ConnectionLost: return "connection_lost";
        case DisconnectReason::Unknown: return "unknown";
    }
    return "unknown";
}

const char* to_string(ChatCategory c) noexcept {
    switch (c) {
        case ChatCategory::Channel: return "channel";
        case ChatCategory::Server: return "server";
        case ChatCategory::Private: return "private";
    }
    return "channel";
}

const char* to_string(JoinCause c) noexcept {
    switch (c) {
        case JoinCause::Moved: return "moved";
        case JoinCause::Connected: return "connected";
        case JoinCause::Disconnected: return "disconnected";
        case JoinCause::Timeout: return "timeout";
        case JoinCause::Kicked: return "kicked";
        case JoinCause::Banned: return "banned";
    }
    return "moved";
}

bool parse_connection_state(std::string_view s, ConnectionState& out) noexcept {
    if (s == "disconnected") { out = ConnectionState::Disconnected; return true; }
    if (s == "connecting") { out = ConnectionState::Connecting; return true; }
    if (s == "connected") { out = ConnectionState::Connected; return true; }
    return false;
}

bool parse_disconnect_reason(std::string_view s, DisconnectReason& out) noexcept {
    if (s == "user") { out = DisconnectReason::User; return true; }
    if (s == "timeout") { out = DisconnectReason::Timeout; return true; }
    if (s == "kicked") { out = DisconnectReason::Kicked; return true; }
    if (s == "banned") { out = DisconnectReason::Banned; return true; }
    if (s == "server_shutdown") { out = DisconnectReason::ServerShutdown; return true; }
    if (s == "connection_lost") { out = DisconnectReason::ConnectionLost; return true; }
    if (s == "unknown") { out = DisconnectReason::Unknown; return true; }
    return false;
}

bool parse_chat_category(std::string_view s, ChatCategory& out) noexcept {
    if (s == "channel") { out = ChatCategory::Channel; return true; }
    if (s == "server") { out = ChatCategory::Server; return true; }
    if (s == "private") { out = ChatCategory::Private; return true; }
    return false;
}

bool parse_join_cause(std::string_view s, JoinCause& out) noexcept {
    if (s == "moved") { out = JoinCause::Moved; return true; }
    if (s == "connected") { out = JoinCause::Connected; return true; }
    if (s == "disconnected") { out = JoinCause::Disconnected; return true; }
    if (s == "timeout") { out = JoinCause::Timeout; return true; }
    if (s == "kicked") { out = JoinCause::Kicked; return true; }
    if (s == "banned") { out = JoinCause::Banned; return true; }
    return false;
}

bool ServerState::operator==(const ServerState& o) const {
    return handler_id == o.handler_id && unique_id == o.unique_id && name == o.name &&
           connection == o.connection;
}

bool ChannelState::operator==(const ChannelState& o) const {
    return id == o.id && parent_id == o.parent_id && name == o.name &&
           parent_name == o.parent_name && path == o.path && topic == o.topic;
}

bool UserState::operator==(const UserState& o) const {
    return client_id == o.client_id && unique_id == o.unique_id && nickname == o.nickname &&
           display_name == o.display_name && talking == o.talking &&
           whispering_to_me == o.whispering_to_me && is_self == o.is_self &&
           input_muted == o.input_muted && output_muted == o.output_muted &&
           input_hardware == o.input_hardware && output_hardware == o.output_hardware &&
           input_deactivated == o.input_deactivated && away == o.away &&
           recording == o.recording && channel_commander == o.channel_commander &&
           priority_speaker == o.priority_speaker && is_talker == o.is_talker &&
           has_avatar == o.has_avatar && locally_muted == o.locally_muted &&
           is_friend == o.is_friend && is_blocked == o.is_blocked &&
           friend_nickname == o.friend_nickname && contact_flag == o.contact_flag &&
           talk_power == o.talk_power &&
           away_message == o.away_message && country == o.country;
}

const UserState* OverlayState::find_user(std::uint16_t cid) const noexcept {
    for (const auto& u : users) {
        if (u.client_id == cid) return &u;
    }
    return nullptr;
}

const UserState* OverlayState::find_user_by_uid(std::string_view uid) const noexcept {
    if (uid.empty()) return nullptr;
    for (const auto& u : users) {
        if (u.unique_id == uid) return &u;
    }
    return nullptr;
}

const UserState* OverlayState::self() const noexcept {
    for (const auto& u : users) {
        if (u.is_self) return &u;
    }
    return find_user_by_uid(self_unique_id);
}

std::size_t OverlayState::talking_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(users.begin(), users.end(), [](const UserState& u) { return u.talking; }));
}

json::Value encode_user(const UserState& u) {
    json::Value o{json::Object{}};
    o.set("client_id", json::Value(static_cast<long long>(u.client_id)));
    o.set("unique_id", json::Value(u.unique_id));
    o.set("nickname", json::Value(u.nickname));
    if (u.display_name != u.nickname) put_str(o, "display_name", u.display_name);
    o.set("talking", json::Value(u.talking));
    if (u.whispering_to_me) o.set("whispering_to_me", json::Value(true));
    if (u.is_self) o.set("is_self", json::Value(true));
    put_opt(o, "input_muted", u.input_muted);
    put_opt(o, "output_muted", u.output_muted);
    put_opt(o, "input_hardware", u.input_hardware);
    put_opt(o, "output_hardware", u.output_hardware);
    put_opt(o, "input_deactivated", u.input_deactivated);
    put_opt(o, "away", u.away);
    put_opt(o, "recording", u.recording);
    put_opt(o, "channel_commander", u.channel_commander);
    put_opt(o, "priority_speaker", u.priority_speaker);
    put_opt(o, "is_talker", u.is_talker);
    put_opt(o, "has_avatar", u.has_avatar);
    put_opt(o, "locally_muted", u.locally_muted);
    put_opt(o, "is_friend", u.is_friend);
    put_opt(o, "is_blocked", u.is_blocked);
    put_str(o, "friend_nickname", u.friend_nickname);
    put_opt_int(o, "contact_flag", u.contact_flag);
    put_opt_int(o, "talk_power", u.talk_power);
    put_str(o, "away_message", u.away_message);
    put_str(o, "country", u.country);
    return o;
}

bool decode_user(const json::Value& v, UserState& out) {
    if (!v.is_object()) return false;
    const json::Value* uid = v.find("unique_id");
    if (uid == nullptr || !uid->is_string() || uid->as_string().empty()) return false;
    out = UserState{};
    out.client_id = static_cast<std::uint16_t>(v.get_int("client_id", 0) & 0xFFFF);
    out.unique_id = uid->as_string();
    out.nickname = v.get_string("nickname");
    out.display_name = v.get_string("display_name", out.nickname);
    if (out.display_name.empty()) out.display_name = out.nickname;
    out.talking = v.get_bool("talking");
    out.whispering_to_me = v.get_bool("whispering_to_me");
    out.is_self = v.get_bool("is_self");
    read_opt(v, "input_muted", out.input_muted);
    read_opt(v, "output_muted", out.output_muted);
    read_opt(v, "input_hardware", out.input_hardware);
    read_opt(v, "output_hardware", out.output_hardware);
    read_opt(v, "input_deactivated", out.input_deactivated);
    read_opt(v, "away", out.away);
    read_opt(v, "recording", out.recording);
    read_opt(v, "channel_commander", out.channel_commander);
    read_opt(v, "priority_speaker", out.priority_speaker);
    read_opt(v, "is_talker", out.is_talker);
    read_opt(v, "has_avatar", out.has_avatar);
    read_opt(v, "locally_muted", out.locally_muted);
    read_opt(v, "is_friend", out.is_friend);
    read_opt(v, "is_blocked", out.is_blocked);
    out.friend_nickname = v.get_string("friend_nickname");
    read_opt_int(v, "contact_flag", out.contact_flag);
    read_opt_int(v, "talk_power", out.talk_power);
    out.away_message = v.get_string("away_message");
    out.country = v.get_string("country");
    return true;
}

json::Value encode_channel(const ChannelState& c) {
    json::Value o{json::Object{}};
    o.set("id", json::Value(static_cast<long long>(c.id)));
    o.set("name", json::Value(c.name));
    if (c.parent_id != 0) o.set("parent_id", json::Value(static_cast<long long>(c.parent_id)));
    put_str(o, "parent_name", c.parent_name);
    put_str(o, "path", c.path);
    put_str(o, "topic", c.topic);
    return o;
}

bool decode_channel(const json::Value& v, ChannelState& out) {
    if (!v.is_object()) return false;
    out = ChannelState{};
    out.id = static_cast<std::uint64_t>(v.get_int("id", 0));
    out.parent_id = static_cast<std::uint64_t>(v.get_int("parent_id", 0));
    out.name = v.get_string("name");
    out.parent_name = v.get_string("parent_name");
    out.path = v.get_string("path", out.name);
    out.topic = v.get_string("topic");
    return true;
}

json::Value encode_server(const ServerState& s) {
    json::Value o{json::Object{}};
    o.set("handler_id", json::Value(static_cast<long long>(s.handler_id)));
    o.set("unique_id", json::Value(s.unique_id));
    o.set("name", json::Value(s.name));
    return o;
}

bool decode_server(const json::Value& v, ServerState& out) {
    if (!v.is_object()) return false;
    out.handler_id = static_cast<std::uint64_t>(v.get_int("handler_id", 0));
    out.unique_id = v.get_string("unique_id");
    out.name = v.get_string("name");
    return true;
}

json::Value encode_snapshot(const OverlayState& s) {
    json::Value o{json::Object{}};
    o.set("connection", json::Value(to_string(s.server.connection)));
    o.set("server", encode_server(s.server));
    o.set("channel", encode_channel(s.channel));
    o.set("self_unique_id", json::Value(s.self_unique_id));
    json::Array arr;
    arr.reserve(s.users.size());
    for (const auto& u : s.users) arr.push_back(encode_user(u));
    o.set("users", json::Value(std::move(arr)));
    return o;
}

bool decode_snapshot(const json::Value& v, OverlayState& out) {
    if (!v.is_object()) return false;
    OverlayState st;
    ConnectionState cs = ConnectionState::Disconnected;
    parse_connection_state(v.get_string("connection", "disconnected"), cs);
    if (const json::Value* sv = v.find("server")) decode_server(*sv, st.server);
    st.server.connection = cs;
    if (const json::Value* cv = v.find("channel")) decode_channel(*cv, st.channel);
    st.self_unique_id = v.get_string("self_unique_id");
    if (const json::Value* uv = v.find("users")) {
        st.users.reserve(uv->as_array().size());
        for (const auto& e : uv->as_array()) {
            UserState u;
            // A single malformed entry is dropped; it must not invalidate the whole snapshot,
            // which would leave the overlay with no state at all.
            if (decode_user(e, u)) st.users.push_back(std::move(u));
        }
    }
    st.synchronised = true;
    out = std::move(st);
    return true;
}

json::Value encode_chat(const ChatMessage& m) {
    json::Value o{json::Object{}};
    o.set("id", json::Value(static_cast<long long>(m.id)));
    o.set("category", json::Value(to_string(m.category)));
    put_str(o, "sender_unique_id", m.sender_unique_id);
    o.set("sender_name", json::Value(m.sender_name));
    put_str(o, "channel_name", m.channel_name);
    o.set("text", json::Value(m.text));
    o.set("timestamp_ms", json::Value(m.timestamp_ms));
    if (m.outgoing) o.set("outgoing", json::Value(true));
    return o;
}

bool decode_chat(const json::Value& v, ChatMessage& out) {
    if (!v.is_object()) return false;
    ChatCategory cat = ChatCategory::Channel;
    if (!parse_chat_category(v.get_string("category", "channel"), cat)) return false;
    out = ChatMessage{};
    out.id = static_cast<std::uint64_t>(v.get_int("id", 0));
    out.category = cat;
    out.sender_unique_id = v.get_string("sender_unique_id");
    out.sender_name = v.get_string("sender_name");
    out.channel_name = v.get_string("channel_name");
    out.text = v.get_string("text");
    out.timestamp_ms = v.get_int("timestamp_ms", 0);
    out.outgoing = v.get_bool("outgoing");
    return true;
}

}  // namespace tsro
