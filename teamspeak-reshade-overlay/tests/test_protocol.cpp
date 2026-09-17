// SPDX-License-Identifier: MIT
#include "tsro/protocol.hpp"
#include "tsro_test.hpp"

using namespace tsro;
using namespace tsro::proto;

namespace {

UserState sample_user() {
    UserState u;
    u.client_id = 17;
    u.unique_id = "kZ9abcDEF/ghi=";
    u.nickname = "Alice";
    u.display_name = "Alice";
    u.talking = true;
    u.input_muted = false;
    u.output_muted = true;
    u.channel_commander = true;
    u.away = false;
    u.talk_power = 75;
    u.country = "GB";
    return u;
}

}  // namespace

TEST(protocol, envelope_round_trips) {
    SpeakingChangedPayload p;
    p.client_id = 17;
    p.unique_id = "kZ9=";
    p.talking = true;
    const std::string line =
        make_line(MessageType::SpeakingChanged, 5, 1737072000123LL, "server-uid", encode(p));
    CHECK_EQ(line.back(), '\n');

    const DecodeResult r = decode(line.substr(0, line.size() - 1));
    CHECK(r.ok());
    CHECK_EQ(r.envelope.seq, 5LL);
    CHECK_EQ(r.envelope.ts, 1737072000123LL);
    CHECK_EQ(r.envelope.server_uid, std::string("server-uid"));
    CHECK(r.envelope.type == MessageType::SpeakingChanged);

    SpeakingChangedPayload back;
    CHECK(decode(r.envelope.data, back));
    CHECK_EQ(back.client_id, 17);
    CHECK_EQ(back.talking, true);
}

TEST(protocol, user_state_round_trips_including_absent_fields) {
    const UserState original = sample_user();
    UserState back;
    CHECK(decode_user(encode_user(original), back));
    CHECK(back == original);

    // A field the SDK could not supply must stay absent, not become false.
    CHECK(!back.recording.has_value());
    CHECK(!back.priority_speaker.has_value());
    CHECK(back.output_muted.has_value());
    CHECK_EQ(*back.output_muted, true);
}

TEST(protocol, absent_is_distinct_from_false_on_the_wire) {
    UserState u = sample_user();
    u.recording.reset();
    const std::string encoded = encode_user(u).dump();
    CHECK(encoded.find("recording") == std::string::npos);

    u.recording = false;
    CHECK(encode_user(u).dump().find("\"recording\":false") != std::string::npos);
}

TEST(protocol, snapshot_round_trips) {
    OverlayState state;
    state.server.unique_id = "srv=";
    state.server.name = "Example";
    state.server.connection = ConnectionState::Connected;
    state.channel.id = 42;
    state.channel.name = "Racing #1";
    state.channel.parent_name = "Games";
    state.self_unique_id = "kZ9abcDEF/ghi=";
    state.users.push_back(sample_user());

    OverlayState back;
    CHECK(decode_snapshot(encode_snapshot(state), back));
    CHECK_EQ(back.users.size(), std::size_t{1});
    CHECK_EQ(back.channel.name, std::string("Racing #1"));
    CHECK(back.server.connection == ConnectionState::Connected);
    CHECK(back.synchronised);
}

TEST(protocol, snapshot_drops_only_the_malformed_user) {
    // One bad entry must not invalidate the whole snapshot, which would leave the overlay with
    // no state at all rather than a nearly-correct one.
    json::Value snapshot{json::Object{}};
    snapshot.set("connection", json::Value("connected"));
    json::Array users;
    users.push_back(encode_user(sample_user()));
    users.push_back(json::Value(json::Object{}));  // no unique_id
    users.push_back(json::Value(42));              // not even an object
    snapshot.set("users", json::Value(std::move(users)));

    OverlayState back;
    CHECK(decode_snapshot(snapshot, back));
    CHECK_EQ(back.users.size(), std::size_t{1});
}

TEST(protocol, rejects_unsupported_versions) {
    const std::string line = R"({"v":99,"seq":0,"ts":0,"type":"hello"})";
    const DecodeResult r = decode(line);
    CHECK(!r.ok());
    CHECK(r.status == DecodeStatus::BadVersion);
    CHECK(!r.fatal());  // a version mismatch is reported, not treated as a corrupt stream
}

TEST(protocol, unknown_message_types_are_ignorable_not_fatal) {
    const DecodeResult r = decode(R"({"v":1,"seq":1,"ts":0,"type":"from_the_future"})");
    CHECK(!r.ok());
    CHECK(r.status == DecodeStatus::UnknownType);
    CHECK(!r.fatal());
}

TEST(protocol, unknown_fields_are_ignored) {
    const DecodeResult r = decode(
        R"({"v":1,"seq":1,"ts":0,"type":"heartbeat","future_field":123,"data":{"uptime_ms":5}})");
    CHECK(r.ok());
    HeartbeatPayload p;
    CHECK(decode(r.envelope.data, p));
    CHECK_EQ(p.uptime_ms, 5LL);
}

TEST(protocol, corrupt_input_is_fatal_for_the_connection) {
    CHECK(decode("this is not json").fatal());
    CHECK(decode("[1,2,3]").fatal());
    CHECK(decode(std::string(kMaxMessageBytes + 1, 'x')).fatal());
}

TEST(protocol, missing_envelope_fields_are_rejected) {
    CHECK(!decode(R"({"seq":1,"type":"hello"})").ok());
    CHECK(!decode(R"({"v":1,"seq":1})").ok());
}

TEST(protocol, chat_subscription_defaults_to_everything_off) {
    // The privacy guarantee: a client that sends an empty or partial configuration receives no
    // chat at all, and in particular no private messages.
    ConfigurationUpdatedPayload p;
    p.chat.channel = true;
    p.chat.priv = true;
    CHECK(decode(json::Value(json::Object{}), p));
    CHECK_EQ(p.chat.channel, false);
    CHECK_EQ(p.chat.server, false);
    CHECK_EQ(p.chat.priv, false);
}

TEST(protocol, chat_subscription_round_trips) {
    ConfigurationUpdatedPayload p;
    p.chat.channel = true;
    p.chat.server = false;
    p.chat.priv = true;
    p.max_chat_length = 512;
    ConfigurationUpdatedPayload back;
    CHECK(decode(encode(p), back));
    CHECK_EQ(back.chat.channel, true);
    CHECK_EQ(back.chat.server, false);
    CHECK_EQ(back.chat.priv, true);
    CHECK_EQ(back.max_chat_length, 512);
}

TEST(protocol, max_chat_length_is_clamped_to_the_protocol_bound) {
    json::Value v{json::Object{}};
    v.set("max_chat_length", json::Value(1000000));
    ConfigurationUpdatedPayload p;
    CHECK(decode(v, p));
    CHECK_EQ(static_cast<std::size_t>(p.max_chat_length), kMaxChatChars);
}

TEST(protocol, client_hello_requires_a_protocol_version) {
    ClientHelloPayload p;
    CHECK(!decode(json::Value(json::Object{}), p));
    json::Value v{json::Object{}};
    v.set("protocol", json::Value(1));
    CHECK(decode(v, p));
    CHECK_EQ(p.protocol, 1);
}

TEST(protocol, whisper_direction_outside_v1_is_rejected) {
    // v1 defines only the incoming direction; a peer claiming otherwise is not one we understand.
    json::Value v{json::Object{}};
    v.set("unique_id", json::Value("kZ9="));
    v.set("direction", json::Value("outgoing"));
    v.set("active", json::Value(true));
    WhisperChangedPayload p;
    CHECK(!decode(v, p));
}

TEST(protocol, identity_payloads_require_a_unique_id) {
    json::Value v{json::Object{}};
    v.set("client_id", json::Value(17));
    SpeakingChangedPayload p;
    CHECK(!decode(v, p));  // client_id alone is session-scoped and not an identity
}

TEST(protocol, message_type_names_are_stable) {
    // These strings are the wire contract; changing one silently would break every older peer.
    CHECK_EQ(std::string(to_string(MessageType::StateSnapshot)), std::string("state_snapshot"));
    CHECK_EQ(std::string(to_string(MessageType::SpeakingChanged)),
             std::string("speaking_changed"));
    CHECK_EQ(std::string(to_string(MessageType::ChatMessageMsg)), std::string("chat_message"));
    CHECK(parse_message_type("user_joined") == MessageType::UserJoined);
    CHECK(parse_message_type("nope") == MessageType::Unknown);
}

TEST(protocol, direction_is_recorded_for_every_type) {
    CHECK(is_server_to_client(MessageType::StateSnapshot));
    CHECK(!is_server_to_client(MessageType::ClientHello));
    CHECK(!is_server_to_client(MessageType::Ping));
}

TEST(protocol, chat_messages_carry_category_and_survive_a_round_trip) {
    ChatMessage m;
    m.id = 9;
    m.category = ChatCategory::Private;
    m.sender_name = "Bob";
    m.sender_unique_id = "bob=";
    m.text = "hello";
    m.timestamp_ms = 1737072000123LL;
    ChatMessage back;
    CHECK(decode_chat(encode_chat(m), back));
    CHECK(back.category == ChatCategory::Private);
    CHECK_EQ(back.text, std::string("hello"));
    CHECK_EQ(back.id, 9ULL);
}
