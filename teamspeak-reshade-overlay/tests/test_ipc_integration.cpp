// SPDX-License-Identifier: MIT
// End-to-end tests over a real kernel transport: a real IpcServer, a real OverlayClient, real
// framing, real reconnection. Nothing here is mocked except the snapshot source, which stands in
// for the TeamSpeak plugin's cached state.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "tsro/ipc_server.hpp"
#include "tsro/overlay_client.hpp"
#include "tsro_test.hpp"

using namespace tsro;
using namespace std::chrono_literals;

namespace {

/// Unique per test so a failing run cannot leave an endpoint that breaks the next one.
std::string unique_endpoint(const char* tag) {
    static std::atomic<int> counter{0};
    const int n = counter.fetch_add(1);
#if defined(_WIN32)
    return "tsro.test." + std::string(tag) + "." + std::to_string(n) + "." +
           std::to_string(static_cast<long long>(::_getpid()));
#else
    const char* tmp = std::getenv("TMPDIR");
    return std::string(tmp != nullptr ? tmp : "/tmp") + "/tsro-test-" + tag + "-" +
           std::to_string(n) + "-" + std::to_string(static_cast<long long>(::getpid())) + ".sock";
#endif
}

/// Spins until `predicate` holds or the budget runs out. Returns whether it held; tests assert
/// on the result rather than sleeping a fixed time and hoping.
template <typename Predicate>
bool wait_for(Predicate predicate, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

/// Stands in for the plugin's snapshot cache: guarded, cheap, and safe to call from IPC threads.
class FakePluginState {
public:
    void set(OverlayState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = std::move(state);
    }
    json::Value snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return encode_snapshot(state_);
    }
    OverlayState get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

private:
    mutable std::mutex mutex_;
    OverlayState state_;
};

UserState make_user(std::uint16_t id, const char* uid, const char* name) {
    UserState u;
    u.client_id = id;
    u.unique_id = uid;
    u.nickname = name;
    u.display_name = name;
    return u;
}

OverlayState populated_state() {
    OverlayState s;
    s.server.handler_id = 1;
    s.server.unique_id = "srvUID=";
    s.server.name = "Example TS";
    s.server.connection = ConnectionState::Connected;
    s.channel.id = 42;
    s.channel.name = "Racing #1";
    s.channel.parent_name = "Games";
    s.self_unique_id = "me=";
    s.users = {make_user(1, "me=", "Me"), make_user(2, "a=", "Alice")};
    s.users[0].is_self = true;
    return s;
}

proto::HelloPayload test_hello() {
    proto::HelloPayload h;
    h.plugin_version = "1.0.0-test";
    h.ts_client_version = "3.6.2";
    h.plugin_api_version = 26;
    h.capabilities = {"chat", "commander", "whisper_incoming"};
    return h;
}

OverlayClientConfig client_config(const std::string& endpoint) {
    OverlayClientConfig c;
    c.endpoint = endpoint;
    c.reconnect_initial_ms = 60;
    c.reconnect_max_ms = 250;
    c.stale_after_ms = 60000;   // staleness is exercised by its own test, not incidentally here
    c.ping_interval_ms = 0;     // pings are exercised separately
    c.hello.client = "test-client";
    c.hello.client_version = "1.0.0";
    c.hello.process = "tsro_tests";
    return c;
}

/// Server + client pair, torn down in the right order however a test exits.
struct Fixture {
    FakePluginState plugin;
    std::unique_ptr<IpcServer> server;
    std::unique_ptr<OverlayClient> client;
    std::string endpoint;

    explicit Fixture(const char* tag, IpcServerConfig overrides = {}) {
        endpoint = unique_endpoint(tag);
        plugin.set(populated_state());
        overrides.endpoint = endpoint;
        if (overrides.heartbeat_ms == 0) overrides.heartbeat_ms = 50;
        server = std::make_unique<IpcServer>(overrides, test_hello(),
                                             [this] { return plugin.snapshot(); });
        std::string error;
        if (!server->start(error)) {
            throw tsro_test::Failure{"could not start the IPC server: " + error};
        }
    }

    void start_client(OverlayClientConfig config) {
        client = std::make_unique<OverlayClient>(std::move(config));
        client->start();
    }

    ~Fixture() {
        if (client) client->stop();
        if (server) server->stop();
    }
};

}  // namespace

TEST(ipc, a_client_connects_and_receives_a_snapshot) {
    Fixture f("connect");
    f.start_client(client_config(f.endpoint));

    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));
    const OverlayFrame& frame = f.client->latest();
    CHECK_EQ(frame.state.channel.name, std::string("Racing #1"));
    CHECK_EQ(frame.state.server.name, std::string("Example TS"));
    CHECK(frame.state.synchronised);
    CHECK(frame.link == LinkState::Connected);
}

TEST(ipc, the_handshake_exposes_plugin_details_for_diagnostics) {
    Fixture f("handshake");
    f.start_client(client_config(f.endpoint));

    CHECK(wait_for([&] { return !f.client->diagnostics().plugin_version.empty(); }));
    const LinkDiagnostics d = f.client->diagnostics();
    CHECK_EQ(d.plugin_version, std::string("1.0.0-test"));
    CHECK_EQ(d.plugin_api_version, 26);
    CHECK_EQ(d.capabilities.size(), std::size_t{3});
    CHECK(wait_for([&] { return f.server->stats().connections_accepted == 1; }));
}

TEST(ipc, events_broadcast_by_the_plugin_reach_the_overlay) {
    Fixture f("events");
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    proto::UserJoinedPayload join;
    join.user = make_user(3, "bob=", "Bob");
    join.cause = JoinCause::Moved;
    f.server->broadcast(proto::MessageType::UserJoined, proto::encode(join), "srvUID=");

    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 3; }));
    CHECK(f.client->latest().state.find_user_by_uid("bob=") != nullptr);
}

TEST(ipc, speaking_changes_propagate) {
    Fixture f("speaking");
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    proto::SpeakingChangedPayload sp;
    sp.client_id = 2;
    sp.unique_id = "a=";
    sp.talking = true;
    f.server->broadcast(proto::MessageType::SpeakingChanged, proto::encode(sp), "srvUID=");

    CHECK(wait_for([&] {
        const UserState* u = f.client->latest().state.find_user_by_uid("a=");
        return u != nullptr && u->talking;
    }));
}

TEST(ipc, semantic_events_are_drained_by_the_renderer) {
    Fixture f("drain");
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    proto::UserJoinedPayload join;
    join.user = make_user(3, "bob=", "Bob");
    f.server->broadcast(proto::MessageType::UserJoined, proto::encode(join), "srvUID=");
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 3; }));

    bool saw_join = false;
    for (const OverlayEvent& e : f.client->drain_events()) {
        if (e.kind == OverlayEventKind::UserJoined && e.display_name == "Bob") saw_join = true;
    }
    CHECK(saw_join);
    CHECK_EQ(f.client->drain_events().size(), std::size_t{0});  // draining is destructive
}

TEST(ipc, chat_is_withheld_until_the_client_subscribes) {
    // The privacy guarantee, verified over the wire rather than asserted in prose.
    Fixture f("chat-privacy");
    OverlayClientConfig config = client_config(f.endpoint);
    config.subscription.chat = proto::ChatSubscription{};  // everything off
    f.start_client(config);
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    ChatMessage priv;
    priv.category = ChatCategory::Private;
    priv.sender_name = "Alice";
    priv.sender_unique_id = "a=";
    priv.text = "this must not be delivered";
    f.server->broadcast_chat(priv, "srvUID=");

    ChatMessage channel;
    channel.category = ChatCategory::Channel;
    channel.sender_name = "Alice";
    channel.text = "this must not be delivered either";
    f.server->broadcast_chat(channel, "srvUID=");

    // Give the transport ample opportunity to deliver something it should not.
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(f.client->latest().chat.size(), std::size_t{0});
}

TEST(ipc, subscribing_to_one_category_does_not_open_the_others) {
    Fixture f("chat-subscribe");
    OverlayClientConfig config = client_config(f.endpoint);
    config.subscription.chat.channel = true;   // channel only
    f.start_client(config);
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    ChatMessage priv;
    priv.category = ChatCategory::Private;
    priv.sender_name = "Alice";
    priv.text = "private";
    f.server->broadcast_chat(priv, "srvUID=");

    ChatMessage channel;
    channel.category = ChatCategory::Channel;
    channel.sender_name = "Alice";
    channel.text = "channel";
    f.server->broadcast_chat(channel, "srvUID=");

    CHECK(wait_for([&] { return f.client->latest().chat.size() == 1; }));
    std::this_thread::sleep_for(200ms);
    const std::vector<ChatMessage> chat = f.client->latest().chat;
    CHECK_EQ(chat.size(), std::size_t{1});
    CHECK_EQ(chat[0].text, std::string("channel"));
}

TEST(ipc, a_subscription_change_takes_effect_on_a_live_connection) {
    Fixture f("chat-resubscribe");
    OverlayClientConfig config = client_config(f.endpoint);
    f.start_client(config);
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    proto::ConfigurationUpdatedPayload update;
    update.chat.channel = true;
    f.client->update_subscription(update);

    // The client flushes the change on its next read, so nudge it with any server traffic.
    CHECK(wait_for([&] {
        ChatMessage m;
        m.category = ChatCategory::Channel;
        m.sender_name = "Alice";
        m.text = "after resubscribe";
        f.server->broadcast_chat(m, "srvUID=");
        return f.client->latest().chat.size() > 0;
    }));
    CHECK_EQ(f.client->latest().chat.back().text, std::string("after resubscribe"));
}

TEST(ipc, the_client_reconnects_after_the_plugin_restarts) {
    Fixture f("reconnect");
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    f.server->stop();
    // On the way down, the user list must be emptied rather than left stale on screen.
    CHECK(wait_for([&] {
        const OverlayFrame& frame = f.client->latest();
        return frame.link != LinkState::Connected && frame.state.users.empty();
    }));

    f.server = std::make_unique<IpcServer>(
        [&] {
            IpcServerConfig c;
            c.endpoint = f.endpoint;
            c.heartbeat_ms = 50;
            return c;
        }(),
        test_hello(), [&f] { return f.plugin.snapshot(); });
    std::string error;
    CHECK(f.server->start(error));

    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }, 10000));
    CHECK(f.client->latest().link == LinkState::Connected);
}

TEST(ipc, a_client_started_before_the_plugin_connects_when_it_appears) {
    // The common real-world ordering: the game launches first, TeamSpeak is opened afterwards.
    const std::string endpoint = unique_endpoint("late-server");
    OverlayClient client(client_config(endpoint));
    client.start();
    CHECK(wait_for([&] { return client.diagnostics().state == LinkState::Backoff; }));

    FakePluginState plugin;
    plugin.set(populated_state());
    IpcServerConfig config;
    config.endpoint = endpoint;
    config.heartbeat_ms = 50;
    IpcServer server(config, test_hello(), [&plugin] { return plugin.snapshot(); });
    std::string error;
    CHECK(server.start(error));

    CHECK(wait_for([&] { return client.latest().state.users.size() == 2; }, 10000));
    client.stop();
    server.stop();
}

TEST(ipc, a_requested_snapshot_reflects_the_plugins_current_state) {
    Fixture f("resnapshot");
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    OverlayState updated = populated_state();
    updated.channel.name = "Pit Lane";
    updated.users.push_back(make_user(9, "carol=", "Carol"));
    f.plugin.set(updated);
    f.server->broadcast_snapshot("srvUID=");

    CHECK(wait_for([&] { return f.client->latest().state.channel.name == "Pit Lane"; }));
    CHECK_EQ(f.client->latest().state.users.size(), std::size_t{3});
}

TEST(ipc, several_clients_are_served_independently) {
    Fixture f("multi");
    f.start_client(client_config(f.endpoint));
    OverlayClient second(client_config(f.endpoint));
    second.start();

    CHECK(wait_for([&] {
        return f.client->latest().state.users.size() == 2 &&
               second.latest().state.users.size() == 2;
    }));
    CHECK(wait_for([&] { return f.server->stats().connected_clients == 2; }));

    proto::UserJoinedPayload join;
    join.user = make_user(3, "bob=", "Bob");
    f.server->broadcast(proto::MessageType::UserJoined, proto::encode(join), "srvUID=");

    CHECK(wait_for([&] {
        return f.client->latest().state.users.size() == 3 &&
               second.latest().state.users.size() == 3;
    }));
    second.stop();
}

TEST(ipc, the_connection_limit_is_enforced) {
    IpcServerConfig config;
    config.max_clients = 1;
    Fixture f("limit", config);
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.server->stats().connected_clients == 1; }));

    OverlayClient rejected(client_config(f.endpoint));
    rejected.start();
    CHECK(wait_for([&] { return f.server->stats().connections_rejected >= 1; }));
    rejected.stop();
}

TEST(ipc, heartbeats_keep_the_state_fresh) {
    Fixture f("heartbeat");
    OverlayClientConfig config = client_config(f.endpoint);
    config.stale_after_ms = 400;  // well above the 50 ms server heartbeat
    f.start_client(config);
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    std::this_thread::sleep_for(1200ms);  // several staleness windows of silence-but-for-heartbeats
    CHECK(!f.client->latest().state.stale);
    CHECK(f.client->latest().link == LinkState::Connected);
}

TEST(ipc, a_ping_round_trip_is_measured) {
    Fixture f("ping");
    OverlayClientConfig config = client_config(f.endpoint);
    config.ping_interval_ms = 50;
    f.start_client(config);
    CHECK(wait_for([&] { return f.client->diagnostics().round_trip_ms >= 0; }));
    CHECK(f.client->diagnostics().round_trip_ms < 5000);
}

TEST(ipc, a_malformed_client_message_does_not_bring_the_server_down) {
    Fixture f("garbage");
    std::string error;
    std::unique_ptr<Connection> raw = connect_client(default_endpoint(f.endpoint), 1000, error);
    CHECK(raw != nullptr);

    const std::string garbage = "this is not json\n{\"v\":1}\n[]\n";
    CHECK(raw->write_all(garbage.data(), garbage.size()));
    std::this_thread::sleep_for(200ms);

    // A well-behaved client connecting afterwards must still be served normally.
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));
    CHECK(f.server->running());
    raw->cancel();
}

TEST(ipc, an_oversized_frame_closes_only_the_offending_connection) {
    Fixture f("oversize");
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    std::string error;
    std::unique_ptr<Connection> raw = connect_client(default_endpoint(f.endpoint), 1000, error);
    CHECK(raw != nullptr);
    const std::string flood(proto::kMaxMessageBytes + 4096, 'x');  // no newline, ever
    raw->write_all(flood.data(), flood.size());

    CHECK(wait_for([&] { return f.server->stats().connected_clients <= 2; }, 3000));

    proto::UserJoinedPayload join;
    join.user = make_user(3, "bob=", "Bob");
    f.server->broadcast(proto::MessageType::UserJoined, proto::encode(join), "srvUID=");
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 3; }));
    raw->cancel();
}

TEST(ipc, an_unsupported_protocol_version_is_rejected_with_an_error) {
    Fixture f("version");
    std::string error;
    std::unique_ptr<Connection> raw = connect_client(default_endpoint(f.endpoint), 1000, error);
    CHECK(raw != nullptr);

    json::Value hello{json::Object{}};
    hello.set("protocol", json::Value(99));
    hello.set("client", json::Value("from-the-future"));
    const std::string line = proto::make_line(proto::MessageType::ClientHello, 0,
                                              proto::now_unix_ms(), "", std::move(hello));
    CHECK(raw->write_all(line.data(), line.size()));

    // Read until the error message arrives (hello and a snapshot precede it).
    std::string received;
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           received.find("unsupported_version") == std::string::npos) {
        const int n = raw->read(buffer, sizeof(buffer));
        if (n <= 0) break;
        received.append(buffer, static_cast<std::size_t>(n));
    }
    CHECK(received.find("unsupported_version") != std::string::npos);
    raw->cancel();
}

TEST(ipc, stopping_the_client_is_prompt_and_clean) {
    Fixture f("stop");
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    const auto start = std::chrono::steady_clock::now();
    f.client->stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 2000);
    CHECK(!f.client->running());
    f.client.reset();
}

TEST(ipc, the_server_survives_a_client_vanishing_mid_stream) {
    Fixture f("abrupt");
    {
        OverlayClient transient(client_config(f.endpoint));
        transient.start();
        CHECK(wait_for([&] { return f.server->stats().connected_clients >= 1; }));
        // Destructor stops it, mimicking a game process exiting.
    }
    CHECK(wait_for([&] { return f.server->stats().connected_clients == 0; }));
    CHECK(f.server->running());

    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));
}

TEST(ipc, a_high_event_rate_is_delivered_without_loss_of_final_state) {
    Fixture f("burst");
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));

    for (int i = 0; i < 500; ++i) {
        proto::SpeakingChangedPayload sp;
        sp.client_id = 2;
        sp.unique_id = "a=";
        sp.talking = (i % 2) == 0;
        f.server->broadcast(proto::MessageType::SpeakingChanged, proto::encode(sp), "srvUID=");
    }
    proto::SpeakingChangedPayload final_state;
    final_state.client_id = 2;
    final_state.unique_id = "a=";
    final_state.talking = true;
    f.server->broadcast(proto::MessageType::SpeakingChanged, proto::encode(final_state),
                        "srvUID=");

    CHECK(wait_for([&] {
        const UserState* u = f.client->latest().state.find_user_by_uid("a=");
        return u != nullptr && u->talking;
    }, 10000));
}

TEST(ipc, the_endpoint_is_reported_for_diagnostics) {
    Fixture f("endpoint");
    CHECK(!f.server->endpoint().empty());
    f.start_client(client_config(f.endpoint));
    CHECK(wait_for([&] { return f.client->latest().state.users.size() == 2; }));
    CHECK(!f.client->diagnostics().endpoint.empty());
    CHECK_EQ(f.server->describe_clients().size(), std::size_t{1});
}
