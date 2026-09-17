// SPDX-License-Identifier: MIT
#include "tsro/state_store.hpp"
#include "tsro_test.hpp"

using namespace tsro;
using namespace tsro::proto;

namespace {

struct Harness {
    StateStore store;
    std::vector<OverlayEvent> events;
    std::int64_t seq = 0;
    std::int64_t clock = 1000;

    StoreAction send(MessageType type, json::Value data) {
        Envelope env;
        env.version = kProtocolVersion;
        env.seq = seq++;
        env.ts = clock;
        env.type = type;
        env.data = std::move(data);
        return store.apply(env, events);
    }

    StoreAction send_raw(MessageType type, std::int64_t explicit_seq, json::Value data) {
        Envelope env;
        env.version = kProtocolVersion;
        env.seq = explicit_seq;
        env.ts = clock;
        env.type = type;
        env.data = std::move(data);
        return store.apply(env, events);
    }

    void hello() {
        HelloPayload h;
        h.plugin_version = "1.0.0";
        h.capabilities = {"chat", "commander"};
        send(MessageType::Hello, encode(h));
    }

    void snapshot(std::vector<UserState> users, const char* channel = "Racing #1") {
        OverlayState st;
        st.server.connection = ConnectionState::Connected;
        st.server.unique_id = "srv=";
        st.server.name = "Example";
        st.channel.id = 42;
        st.channel.name = channel;
        st.users = std::move(users);
        send(MessageType::StateSnapshot, encode_snapshot(st));
    }

    bool saw(OverlayEventKind kind) const {
        for (const auto& e : events) {
            if (e.kind == kind) return true;
        }
        return false;
    }
    std::size_t count(OverlayEventKind kind) const {
        std::size_t n = 0;
        for (const auto& e : events) {
            if (e.kind == kind) ++n;
        }
        return n;
    }
};

UserState user(std::uint16_t id, const char* uid, const char* name) {
    UserState u;
    u.client_id = id;
    u.unique_id = uid;
    u.nickname = name;
    u.display_name = name;
    return u;
}

}  // namespace

TEST(state_store, applies_a_snapshot) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice"), user(2, "b=", "Bob")});
    CHECK_EQ(h.store.state().users.size(), std::size_t{2});
    CHECK_EQ(h.store.state().channel.name, std::string("Racing #1"));
    CHECK(h.store.state().synchronised);
    CHECK(h.store.handshaken());
}

TEST(state_store, capabilities_are_queryable) {
    Harness h;
    h.hello();
    CHECK(h.store.has_capability("commander"));
    CHECK(!h.store.has_capability("telepathy"));
}

TEST(state_store, join_and_leave_update_membership) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    UserJoinedPayload join;
    join.user = user(2, "b=", "Bob");
    h.send(MessageType::UserJoined, encode(join));
    CHECK_EQ(h.store.state().users.size(), std::size_t{2});
    CHECK(h.saw(OverlayEventKind::UserJoined));

    UserLeftPayload left;
    left.user = user(1, "a=", "Alice");
    h.send(MessageType::UserLeft, encode(left));
    CHECK_EQ(h.store.state().users.size(), std::size_t{1});
    CHECK_EQ(h.store.state().users[0].unique_id, std::string("b="));
}

TEST(state_store, a_repeated_join_does_not_duplicate_the_row) {
    Harness h;
    h.hello();
    h.snapshot({});
    UserJoinedPayload join;
    join.user = user(2, "b=", "Bob");
    h.send(MessageType::UserJoined, encode(join));
    h.send(MessageType::UserJoined, encode(join));
    CHECK_EQ(h.store.state().users.size(), std::size_t{1});
}

TEST(state_store, identity_is_keyed_on_unique_id_not_client_id) {
    // TeamSpeak recycles client ids within a session. A speaking event whose recycled id now
    // belongs to someone else must not light up the wrong person.
    Harness h;
    h.hello();
    h.snapshot({user(1, "alice=", "Alice"), user(2, "bob=", "Bob")});

    SpeakingChangedPayload sp;
    sp.client_id = 1;          // Alice's id...
    sp.unique_id = "bob=";     // ...but Bob's identity
    sp.talking = true;
    h.send(MessageType::SpeakingChanged, encode(sp));

    CHECK_EQ(h.store.state().find_user_by_uid("bob=")->talking, true);
    CHECK_EQ(h.store.state().find_user_by_uid("alice=")->talking, false);
}

TEST(state_store, speaking_transitions_emit_events_only_on_change) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    SpeakingChangedPayload sp;
    sp.client_id = 1;
    sp.unique_id = "a=";
    sp.talking = true;
    h.send(MessageType::SpeakingChanged, encode(sp));
    h.send(MessageType::SpeakingChanged, encode(sp));  // repeat: no second event
    CHECK_EQ(h.count(OverlayEventKind::SpeakingStarted), std::size_t{1});

    sp.talking = false;
    h.send(MessageType::SpeakingChanged, encode(sp));
    CHECK_EQ(h.count(OverlayEventKind::SpeakingStopped), std::size_t{1});
    CHECK_EQ(h.store.state().users[0].talking, false);
}

TEST(state_store, mute_changes_do_not_conflate_microphone_and_speakers) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    MuteChangedPayload m;
    m.client_id = 1;
    m.unique_id = "a=";
    m.output_muted = true;  // speakers only
    h.send(MessageType::MuteChanged, encode(m));

    const UserState* u = h.store.state().find_user_by_uid("a=");
    CHECK_EQ(u->output_muted.value_or(false), true);
    CHECK_EQ(u->input_muted.has_value(), false);  // untouched, still unknown
}

TEST(state_store, omitted_mute_fields_do_not_clear_known_state) {
    Harness h;
    h.hello();
    UserState alice = user(1, "a=", "Alice");
    alice.input_muted = true;
    h.snapshot({alice});

    MuteChangedPayload m;
    m.client_id = 1;
    m.unique_id = "a=";
    m.output_muted = true;  // input_muted omitted
    h.send(MessageType::MuteChanged, encode(m));

    CHECK_EQ(h.store.state().find_user_by_uid("a=")->input_muted.value_or(false), true);
}

TEST(state_store, commander_changes_emit_an_event) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    CommanderChangedPayload c;
    c.client_id = 1;
    c.unique_id = "a=";
    c.channel_commander = true;
    h.send(MessageType::CommanderChanged, encode(c));
    CHECK(h.saw(OverlayEventKind::CommanderChanged));
    CHECK_EQ(h.store.state().find_user_by_uid("a=")->channel_commander.value_or(false), true);

    const std::size_t before = h.count(OverlayEventKind::CommanderChanged);
    h.send(MessageType::CommanderChanged, encode(c));  // unchanged: no event
    CHECK_EQ(h.count(OverlayEventKind::CommanderChanged), before);
}

TEST(state_store, user_updated_does_not_clobber_newer_speaking_state) {
    // user_updated carries a full UserState built from a snapshot that may predate the far more
    // frequent speaking events. Talking must stay owned by speaking_changed.
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    SpeakingChangedPayload sp;
    sp.client_id = 1;
    sp.unique_id = "a=";
    sp.talking = true;
    h.send(MessageType::SpeakingChanged, encode(sp));

    UserUpdatedPayload up;
    up.user = user(1, "a=", "Alice");  // talking == false in this payload
    up.user.away = true;
    up.changed = {"away"};
    h.send(MessageType::UserUpdated, encode(up));

    const UserState* u = h.store.state().find_user_by_uid("a=");
    CHECK_EQ(u->talking, true);
    CHECK_EQ(u->away.value_or(false), true);
}

TEST(state_store, duplicate_sequence_numbers_are_dropped) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    UserJoinedPayload join;
    join.user = user(2, "b=", "Bob");
    h.send_raw(MessageType::UserJoined, 5, encode(join));
    h.send_raw(MessageType::UserJoined, 5, encode(join));
    h.send_raw(MessageType::UserJoined, 3, encode(join));  // older
    CHECK_EQ(h.store.stats().duplicates_dropped, 2ULL);
    CHECK_EQ(h.store.state().users.size(), std::size_t{2});
}

TEST(state_store, a_sequence_gap_requests_a_resynchronisation) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    UserJoinedPayload join;
    join.user = user(2, "b=", "Bob");
    const StoreAction action = h.send_raw(MessageType::UserJoined, 99, encode(join));
    CHECK(action == StoreAction::RequestSnapshot);
    CHECK_EQ(h.store.stats().gaps_detected, 1ULL);
    CHECK(h.saw(OverlayEventKind::Desynchronised));
}

TEST(state_store, going_stale_clears_liveness_and_asks_for_a_snapshot) {
    // The brief's hard requirement: never present an outdated user list indefinitely.
    Harness h;
    StoreConfig cfg;
    cfg.stale_after_ms = 1000;
    h.store.set_config(cfg);
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    CHECK(h.store.tick(h.clock + 500, h.events) == StoreAction::None);
    CHECK(!h.store.state().stale);

    CHECK(h.store.tick(h.clock + 5000, h.events) == StoreAction::RequestSnapshot);
    CHECK(h.store.state().stale);

    // The request is not repeated every tick; one resync attempt per staleness episode.
    CHECK(h.store.tick(h.clock + 6000, h.events) == StoreAction::None);
}

TEST(state_store, fresh_data_clears_the_stale_flag) {
    Harness h;
    StoreConfig cfg;
    cfg.stale_after_ms = 1000;
    h.store.set_config(cfg);
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});
    h.store.tick(h.clock + 5000, h.events);
    CHECK(h.store.state().stale);

    h.clock += 6000;
    h.snapshot({user(1, "a=", "Alice")});
    CHECK(!h.store.state().stale);
}

TEST(state_store, disconnection_empties_the_user_list) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice"), user(2, "b=", "Bob")});
    h.store.on_disconnected(h.clock, h.events);

    CHECK_EQ(h.store.state().users.size(), std::size_t{0});
    CHECK(!h.store.state().synchronised);
    CHECK(h.store.state().stale);
    CHECK(h.store.state().server.connection == ConnectionState::Disconnected);
    CHECK(h.saw(OverlayEventKind::ConnectionChanged));
}

TEST(state_store, connection_loss_via_the_protocol_also_empties_the_list) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    ConnectionChangedPayload c;
    c.connection = ConnectionState::Disconnected;
    c.reason = DisconnectReason::ConnectionLost;
    h.send(MessageType::ConnectionChanged, encode(c));
    CHECK_EQ(h.store.state().users.size(), std::size_t{0});
}

TEST(state_store, repeated_connection_states_do_not_repeat_notifications) {
    Harness h;
    h.hello();
    h.snapshot({});
    ConnectionChangedPayload c;
    c.connection = ConnectionState::Connected;
    h.send(MessageType::ConnectionChanged, encode(c));
    const std::size_t before = h.count(OverlayEventKind::ConnectionChanged);
    h.send(MessageType::ConnectionChanged, encode(c));
    h.send(MessageType::ConnectionChanged, encode(c));
    CHECK_EQ(h.count(OverlayEventKind::ConnectionChanged), before);
}

TEST(state_store, a_channel_change_invalidates_membership_until_resynchronised) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice"), user(2, "b=", "Bob")});

    ChannelChangedPayload cc;
    cc.to.id = 77;
    cc.to.name = "Lobby";
    ChannelState from;
    from.id = 42;
    from.name = "Racing #1";
    cc.from = from;
    cc.user_count = 1;
    const StoreAction action = h.send(MessageType::ChannelChanged, encode(cc));

    CHECK(action == StoreAction::RequestSnapshot);
    CHECK_EQ(h.store.state().channel.name, std::string("Lobby"));
    CHECK_EQ(h.store.state().users.size(), std::size_t{0});
    CHECK(!h.store.state().synchronised);
    CHECK(h.saw(OverlayEventKind::ChannelChanged));
}

TEST(state_store, a_snapshot_onto_a_different_channel_is_reported_as_a_change) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")}, "Racing #1");
    const std::size_t before = h.count(OverlayEventKind::ChannelChanged);

    OverlayState st;
    st.server.connection = ConnectionState::Connected;
    st.channel.id = 99;
    st.channel.name = "Pit Lane";
    h.send(MessageType::StateSnapshot, encode_snapshot(st));
    CHECK_EQ(h.count(OverlayEventKind::ChannelChanged), before + 1);
}

TEST(state_store, chat_history_is_bounded) {
    Harness h;
    StoreConfig cfg;
    cfg.chat_history = 3;
    h.store.set_config(cfg);
    h.hello();
    h.snapshot({});

    for (int i = 0; i < 10; ++i) {
        ChatMessage m;
        m.id = static_cast<std::uint64_t>(i);
        m.category = ChatCategory::Channel;
        m.sender_name = "Alice";
        m.text = "message " + std::to_string(i);
        h.send(MessageType::ChatMessageMsg, encode_chat(m));
    }
    CHECK_EQ(h.store.chat().size(), std::size_t{3});
    CHECK_EQ(h.store.chat().back().text, std::string("message 9"));
}

TEST(state_store, chat_text_is_clamped) {
    Harness h;
    h.hello();
    h.snapshot({});
    ChatMessage m;
    m.category = ChatCategory::Channel;
    m.sender_name = "Alice";
    m.text = std::string(5000, 'x');
    h.send(MessageType::ChatMessageMsg, encode_chat(m));
    CHECK(h.store.chat().back().text.size() <= kMaxChatChars);
}

TEST(state_store, a_client_direction_message_is_a_protocol_error) {
    Harness h;
    h.hello();
    Envelope env;
    env.version = kProtocolVersion;
    env.seq = 100;
    env.type = MessageType::ClientHello;
    std::vector<OverlayEvent> events;
    CHECK(h.store.apply(env, events) == StoreAction::Disconnect);
}

TEST(state_store, a_fatal_error_message_disconnects) {
    Harness h;
    h.hello();
    ErrorPayload e;
    e.code = "unsupported_version";
    e.fatal = true;
    CHECK(h.send(MessageType::Error, encode(e)) == StoreAction::Disconnect);
}

TEST(state_store, a_non_fatal_error_does_not_disconnect) {
    Harness h;
    h.hello();
    ErrorPayload e;
    e.code = "malformed";
    e.fatal = false;
    CHECK(h.send(MessageType::Error, encode(e)) == StoreAction::None);
}

TEST(state_store, a_hello_with_an_incompatible_range_disconnects) {
    Harness h;
    HelloPayload hp;
    hp.protocol_min = 7;
    hp.protocol_max = 9;
    CHECK(h.send(MessageType::Hello, encode(hp)) == StoreAction::Disconnect);
}

TEST(state_store, malformed_payloads_are_counted_and_skipped) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});
    json::Value garbage{json::Object{}};
    garbage.set("client_id", json::Value(1));  // no unique_id
    h.send(MessageType::SpeakingChanged, std::move(garbage));
    CHECK_EQ(h.store.stats().malformed_payloads, 1ULL);
    CHECK_EQ(h.store.state().users.size(), std::size_t{1});
}

TEST(state_store, an_oversized_snapshot_is_truncated) {
    Harness h;
    h.hello();
    std::vector<UserState> many;
    for (int i = 0; i < 700; ++i) {
        many.push_back(user(static_cast<std::uint16_t>(i), ("u" + std::to_string(i) + "=").c_str(),
                            "User"));
    }
    h.snapshot(std::move(many));
    CHECK_EQ(h.store.state().users.size(), kMaxUsersPerSnapshot);
}

TEST(state_store, a_new_hello_resets_sequence_tracking) {
    // A reconnect restarts sequencing at 0; that must not look like a flood of duplicates.
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});
    UserJoinedPayload joined;
    joined.user = user(2, "b=", "Bob");
    joined.cause = JoinCause::Moved;
    h.send_raw(MessageType::UserJoined, 50, encode(joined));

    h.seq = 0;
    h.hello();
    const std::uint64_t dupes = h.store.stats().duplicates_dropped;
    h.snapshot({user(1, "a=", "Alice")});
    CHECK_EQ(h.store.stats().duplicates_dropped, dupes);
    CHECK_EQ(h.store.state().users.size(), std::size_t{1});
}

TEST(state_store, whisper_state_is_tracked_separately_from_speaking) {
    Harness h;
    h.hello();
    h.snapshot({user(1, "a=", "Alice")});

    SpeakingChangedPayload sp;
    sp.client_id = 1;
    sp.unique_id = "a=";
    sp.talking = true;
    sp.whisper = true;
    h.send(MessageType::SpeakingChanged, encode(sp));

    const UserState* u = h.store.state().find_user_by_uid("a=");
    CHECK_EQ(u->talking, true);
    CHECK_EQ(u->whispering_to_me, true);
    CHECK(h.saw(OverlayEventKind::WhisperStarted));

    sp.talking = false;
    sp.whisper = false;
    h.send(MessageType::SpeakingChanged, encode(sp));
    CHECK_EQ(h.store.state().find_user_by_uid("a=")->whispering_to_me, false);
    CHECK(h.saw(OverlayEventKind::WhisperStopped));
}
