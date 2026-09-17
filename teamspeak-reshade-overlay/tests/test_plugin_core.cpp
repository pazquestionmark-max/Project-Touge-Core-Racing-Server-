// SPDX-License-Identifier: MIT
// Exercises the plugin's event collection against a scripted TeamSpeak client, with a real
// OverlayClient consuming the far end of a real socket. These tests cover the integration matrix
// in docs/testing.md §1 that can be automated without a TeamSpeak installation.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "plugin_core.hpp"
#include "ts_query_stub.hpp"
#include "tsro/overlay_client.hpp"
#include "tsro_test.hpp"

using namespace tsro;
using namespace tsro::plugin;
using namespace std::chrono_literals;

namespace {

std::string unique_endpoint(const char* tag) {
    static std::atomic<int> counter{0};
#if defined(_WIN32)
    return "tsro.plugintest." + std::string(tag) + "." +
           std::to_string(counter.fetch_add(1)) + "." +
           std::to_string(static_cast<long long>(::_getpid()));
#else
    const char* tmp = std::getenv("TMPDIR");
    return std::string(tmp != nullptr ? tmp : "/tmp") + "/tsro-plugin-" + tag + "-" +
           std::to_string(counter.fetch_add(1)) + "-" +
           std::to_string(static_cast<long long>(::getpid())) + ".sock";
#endif
}

template <typename Predicate>
bool wait_for(Predicate predicate, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

constexpr std::uint64_t kServer = 1;
constexpr std::uint64_t kGames = 7;     // parent channel
constexpr std::uint64_t kRacing = 42;   // where the local user sits
constexpr std::uint64_t kLobby = 43;
constexpr std::uint16_t kMe = 100;
constexpr std::uint16_t kAlice = 101;
constexpr std::uint16_t kBob = 102;

/// A scripted client: two channels, the local user and Alice in Racing, Bob in the Lobby.
FakeTsQuery make_client() {
    FakeTsQuery q;
    FakeServer server;
    server.name = "Example TS";
    server.unique_id = "srvUID=";
    server.own_client_id = kMe;
    server.channels[kGames] = FakeChannel{"Games", "", 0};
    server.channels[kRacing] = FakeChannel{"Racing #1", "Drive fast", kGames};
    server.channels[kLobby] = FakeChannel{"Lobby", "", kGames};

    FakeClient me;
    me.display_name = "Me";
    me.channel = kRacing;
    me.text[UserText::UniqueIdentifier] = "me=";
    me.text[UserText::Nickname] = "Me";
    me.flags[UserFlag::InputMuted] = 0;
    me.flags[UserFlag::OutputMuted] = 0;
    me.flags[UserFlag::InputHardware] = 1;
    me.flags[UserFlag::OutputHardware] = 1;
    me.flags[UserFlag::InputDeactivated] = 0;
    server.clients[kMe] = me;

    FakeClient alice;
    alice.display_name = "Alice";
    alice.channel = kRacing;
    alice.text[UserText::UniqueIdentifier] = "alice=";
    alice.text[UserText::Nickname] = "Alice";
    alice.flags[UserFlag::InputMuted] = 0;
    alice.flags[UserFlag::OutputMuted] = 0;
    alice.flags[UserFlag::ChannelCommander] = 0;
    alice.flags[UserFlag::Away] = 0;
    alice.flags[UserFlag::TalkPower] = 50;
    server.clients[kAlice] = alice;

    FakeClient bob;
    bob.display_name = "Bob";
    bob.channel = kLobby;
    bob.text[UserText::UniqueIdentifier] = "bob=";
    bob.text[UserText::Nickname] = "Bob";
    bob.flags[UserFlag::InputMuted] = 0;
    server.clients[kBob] = bob;

    q.servers[kServer] = server;
    return q;
}

/// Plugin core plus a real overlay client over a real socket.
struct Harness {
    FakeTsQuery client = make_client();
    std::unique_ptr<PluginCore> core;
    std::unique_ptr<OverlayClient> overlay;
    std::string endpoint;

    explicit Harness(const char* tag) {
        endpoint = unique_endpoint(tag);
        PluginCoreOptions options;
        options.plugin_version = "1.0.0-test";
        options.plugin_api_version = 26;
        options.ipc.endpoint = endpoint;
        options.ipc.heartbeat_ms = 50;
        core = std::make_unique<PluginCore>(client, std::move(options));
        std::string error;
        if (!core->start(error)) throw tsro_test::Failure{"plugin core failed to start: " + error};
    }

    void connect_overlay() {
        OverlayClientConfig config;
        config.endpoint = endpoint;
        config.reconnect_initial_ms = 60;
        config.reconnect_max_ms = 250;
        config.stale_after_ms = 60000;
        config.ping_interval_ms = 0;
        config.subscription.chat.channel = true;  // channel chat only; private stays off
        overlay = std::make_unique<OverlayClient>(config);
        overlay->start();
    }

    /// Brings the plugin to a connected, synchronised state and waits for the overlay to agree.
    void go_live() {
        core->on_connect_status_changed(kServer, TsConnectStatus::Connected, 0);
        connect_overlay();
        if (!wait_for([&] { return overlay->latest().state.users.size() == 2; })) {
            throw tsro_test::Failure{"overlay did not receive the initial roster"};
        }
    }

    const OverlayState& seen() { return overlay->latest().state; }

    ~Harness() {
        if (overlay) overlay->stop();
        if (core) core->stop();
    }
};

}  // namespace

TEST(plugin, resynchronise_builds_the_local_users_channel) {
    FakeTsQuery client = make_client();
    TsState state(client);
    state.resynchronise(kServer);

    CHECK_EQ(state.state().channel.name, std::string("Racing #1"));
    CHECK_EQ(state.state().channel.parent_name, std::string("Games"));
    CHECK_EQ(state.state().channel.path, std::string("Games/Racing #1"));
    CHECK_EQ(state.state().channel.topic, std::string("Drive fast"));
    CHECK_EQ(state.state().server.name, std::string("Example TS"));
    CHECK_EQ(state.state().self_unique_id, std::string("me="));
    // Bob is in another channel and must not appear.
    CHECK_EQ(state.state().users.size(), std::size_t{2});
    CHECK(state.state().find_user_by_uid("bob=") == nullptr);
}

TEST(plugin, the_local_user_is_marked_as_self) {
    FakeTsQuery client = make_client();
    TsState state(client);
    state.resynchronise(kServer);
    const UserState* me = state.state().find_user_by_uid("me=");
    CHECK(me != nullptr);
    CHECK(me->is_self);
    CHECK(state.state().find_user_by_uid("alice=")->is_self == false);
}

TEST(plugin, properties_teamspeak_cannot_supply_are_left_absent) {
    // The scripted Alice has no recording or priority-speaker property, mimicking a client for
    // which TeamSpeak returns an error. Those must be absent, not false.
    FakeTsQuery client = make_client();
    TsState state(client);
    state.resynchronise(kServer);
    const UserState* alice = state.state().find_user_by_uid("alice=");
    CHECK(!alice->recording.has_value());
    CHECK(!alice->priority_speaker.has_value());
    CHECK(alice->channel_commander.has_value());
    CHECK_EQ(*alice->channel_commander, false);
}

TEST(plugin, input_deactivated_is_read_only_for_the_local_user) {
    // The SDK documents CLIENT_INPUT_DEACTIVATED as own-client-only and CLIENT_IS_MUTED as
    // other-clients-only. Reading either from the wrong side would be inventing data.
    FakeTsQuery client = make_client();
    client.servers[kServer].clients[kAlice].flags[UserFlag::InputDeactivated] = 1;
    client.servers[kServer].clients[kAlice].flags[UserFlag::LocallyMuted] = 1;
    client.servers[kServer].clients[kMe].flags[UserFlag::LocallyMuted] = 1;

    TsState state(client);
    state.resynchronise(kServer);
    CHECK(!state.state().find_user_by_uid("alice=")->input_deactivated.has_value());
    CHECK_EQ(state.state().find_user_by_uid("alice=")->locally_muted.value_or(false), true);
    CHECK(state.state().find_user_by_uid("me=")->input_deactivated.has_value());
    CHECK(!state.state().find_user_by_uid("me=")->locally_muted.has_value());
}

TEST(plugin, a_client_without_a_unique_identifier_is_skipped) {
    FakeTsQuery client = make_client();
    client.servers[kServer].clients[kAlice].text.erase(UserText::UniqueIdentifier);
    TsState state(client);
    state.resynchronise(kServer);
    CHECK_EQ(state.state().users.size(), std::size_t{1});
}

TEST(plugin, refresh_user_reports_exactly_what_changed) {
    FakeTsQuery client = make_client();
    TsState state(client);
    state.resynchronise(kServer);

    CHECK_EQ(state.refresh_user(kServer, kAlice).size(), std::size_t{0});  // nothing changed

    client.servers[kServer].clients[kAlice].flags[UserFlag::Away] = 1;
    client.servers[kServer].clients[kAlice].flags[UserFlag::TalkPower] = 90;
    const std::vector<std::string> changed = state.refresh_user(kServer, kAlice);
    CHECK_EQ(changed.size(), std::size_t{2});
    CHECK(std::find(changed.begin(), changed.end(), "away") != changed.end());
    CHECK(std::find(changed.begin(), changed.end(), "talk_power") != changed.end());
}

TEST(plugin, resynchronising_preserves_speaking_state) {
    // Re-reading the roster must not blink every speaking indicator off.
    FakeTsQuery client = make_client();
    TsState state(client);
    state.resynchronise(kServer);
    CHECK(state.set_talking(kAlice, true, false));

    state.resynchronise(kServer);
    CHECK_EQ(state.state().find_user_by_uid("alice=")->talking, true);
}

TEST(plugin, refreshing_a_user_does_not_clobber_speaking_state) {
    FakeTsQuery client = make_client();
    TsState state(client);
    state.resynchronise(kServer);
    state.set_talking(kAlice, true, true);

    client.servers[kServer].clients[kAlice].flags[UserFlag::Away] = 1;
    state.refresh_user(kServer, kAlice);

    const UserState* alice = state.state().find_user_by_uid("alice=");
    CHECK_EQ(alice->talking, true);
    CHECK_EQ(alice->whispering_to_me, true);
    CHECK_EQ(alice->away.value_or(false), true);
}

TEST(plugin, the_snapshot_cache_is_refreshed_without_touching_teamspeak) {
    FakeTsQuery client = make_client();
    TsState state(client);
    state.resynchronise(kServer);

    OverlayState decoded;
    CHECK(decode_snapshot(state.snapshot(), decoded));
    CHECK_EQ(decoded.users.size(), std::size_t{2});
    CHECK_EQ(decoded.channel.name, std::string("Racing #1"));
}

TEST(plugin, connecting_delivers_the_roster_to_the_overlay) {
    Harness h("connect");
    h.go_live();
    CHECK_EQ(h.seen().channel.name, std::string("Racing #1"));
    CHECK_EQ(h.seen().server.name, std::string("Example TS"));
    CHECK(h.seen().find_user_by_uid("alice=") != nullptr);
    CHECK(h.seen().find_user_by_uid("bob=") == nullptr);
}

TEST(plugin, a_user_joining_our_channel_reaches_the_overlay) {
    Harness h("join");
    h.go_live();

    h.client.servers[kServer].clients[kBob].channel = kRacing;
    h.core->on_client_moved(kServer, kBob, kLobby, kRacing, MoveCause::Moved);

    CHECK(wait_for([&] { return h.seen().find_user_by_uid("bob=") != nullptr; }));
    CHECK_EQ(h.seen().users.size(), std::size_t{3});
}

TEST(plugin, a_user_leaving_our_channel_reaches_the_overlay) {
    Harness h("leave");
    h.go_live();

    h.client.servers[kServer].clients[kAlice].channel = kLobby;
    h.core->on_client_moved(kServer, kAlice, kRacing, kLobby, MoveCause::Moved);

    CHECK(wait_for([&] { return h.seen().find_user_by_uid("alice=") == nullptr; }));
    CHECK_EQ(h.seen().users.size(), std::size_t{1});
}

TEST(plugin, movement_between_channels_we_are_not_in_is_ignored) {
    Harness h("elsewhere");
    h.go_live();
    const std::size_t before = h.seen().users.size();

    h.core->on_client_moved(kServer, kBob, kLobby, 999, MoveCause::Moved);
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(h.seen().users.size(), before);
}

TEST(plugin, the_local_user_switching_channels_updates_the_title_immediately) {
    // The brief's explicit requirement: the channel name must update as soon as we move.
    Harness h("switch");
    h.go_live();

    h.client.servers[kServer].clients[kMe].channel = kLobby;
    h.core->on_client_moved(kServer, kMe, kRacing, kLobby, MoveCause::Moved);

    CHECK(wait_for([&] { return h.seen().channel.name == "Lobby"; }));
    // Membership follows the move: Bob is in the Lobby, Alice is not.
    CHECK(wait_for([&] { return h.seen().find_user_by_uid("bob=") != nullptr; }));
    CHECK(h.seen().find_user_by_uid("alice=") == nullptr);
}

TEST(plugin, speaking_state_reaches_the_overlay) {
    Harness h("talk");
    h.go_live();

    h.core->on_talk_status_changed(kServer, TsTalkStatus::Talking, false, kAlice);
    CHECK(wait_for([&] {
        const UserState* a = h.seen().find_user_by_uid("alice=");
        return a != nullptr && a->talking;
    }));

    h.core->on_talk_status_changed(kServer, TsTalkStatus::NotTalking, false, kAlice);
    CHECK(wait_for([&] { return !h.seen().find_user_by_uid("alice=")->talking; }));
}

TEST(plugin, talking_while_disabled_is_not_reported_as_speaking) {
    // The user's microphone is producing audio we are not transmitting; showing them as
    // speaking would be wrong.
    Harness h("talk-disabled");
    h.go_live();
    h.core->on_talk_status_changed(kServer, TsTalkStatus::TalkingWhileDisabled, false, kAlice);
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(h.seen().find_user_by_uid("alice=")->talking, false);
}

TEST(plugin, an_incoming_whisper_is_distinguished_from_channel_speech) {
    Harness h("whisper");
    h.go_live();
    h.core->on_talk_status_changed(kServer, TsTalkStatus::Talking, true, kAlice);

    CHECK(wait_for([&] {
        const UserState* a = h.seen().find_user_by_uid("alice=");
        return a != nullptr && a->talking && a->whispering_to_me;
    }));

    h.core->on_talk_status_changed(kServer, TsTalkStatus::NotTalking, false, kAlice);
    CHECK(wait_for([&] { return !h.seen().find_user_by_uid("alice=")->whispering_to_me; }));
}

TEST(plugin, channel_commander_changes_reach_the_overlay) {
    Harness h("commander");
    h.go_live();

    h.client.servers[kServer].clients[kAlice].flags[UserFlag::ChannelCommander] = 1;
    h.core->on_client_updated(kServer, kAlice);

    CHECK(wait_for([&] {
        const UserState* a = h.seen().find_user_by_uid("alice=");
        return a != nullptr && a->channel_commander.value_or(false);
    }));
}

TEST(plugin, microphone_and_speaker_mute_arrive_as_separate_states) {
    Harness h("mute");
    h.go_live();

    h.client.servers[kServer].clients[kAlice].flags[UserFlag::OutputMuted] = 1;
    h.core->on_client_updated(kServer, kAlice);
    CHECK(wait_for([&] {
        const UserState* a = h.seen().find_user_by_uid("alice=");
        return a != nullptr && a->output_muted.value_or(false);
    }));
    CHECK_EQ(h.seen().find_user_by_uid("alice=")->input_muted.value_or(true), false);

    h.client.servers[kServer].clients[kAlice].flags[UserFlag::InputMuted] = 1;
    h.core->on_client_updated(kServer, kAlice);
    CHECK(wait_for([&] {
        return h.seen().find_user_by_uid("alice=")->input_muted.value_or(false);
    }));
}

TEST(plugin, away_state_reaches_the_overlay) {
    Harness h("away");
    h.go_live();
    h.client.servers[kServer].clients[kAlice].flags[UserFlag::Away] = 1;
    h.client.servers[kServer].clients[kAlice].text[UserText::AwayMessage] = "brb";
    h.core->on_client_updated(kServer, kAlice);

    CHECK(wait_for([&] {
        const UserState* a = h.seen().find_user_by_uid("alice=");
        return a != nullptr && a->away.value_or(false) && a->away_message == "brb";
    }));
}

TEST(plugin, channel_chat_is_forwarded_when_subscribed) {
    Harness h("chat-channel");
    h.go_live();
    h.core->on_text_message(kServer, TsTextTarget::Channel, kAlice, "Alice", "alice=", "gg");
    CHECK(wait_for([&] { return h.overlay->latest().chat.size() == 1; }));
    CHECK_EQ(h.overlay->latest().chat[0].text, std::string("gg"));
    CHECK(h.overlay->latest().chat[0].category == ChatCategory::Channel);
}

TEST(plugin, private_messages_are_not_forwarded_to_an_unsubscribed_client) {
    // The privacy guarantee, from the TeamSpeak callback all the way to the overlay.
    Harness h("chat-private");
    h.go_live();
    h.core->on_text_message(kServer, TsTextTarget::Client, kAlice, "Alice", "alice=", "secret");
    h.core->on_text_message(kServer, TsTextTarget::Server, kAlice, "Alice", "alice=", "notice");
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(h.overlay->latest().chat.size(), std::size_t{0});
}

TEST(plugin, overlong_chat_is_clamped_before_it_leaves_the_plugin) {
    Harness h("chat-clamp");
    h.go_live();
    h.core->on_text_message(kServer, TsTextTarget::Channel, kAlice, "Alice", "alice=",
                            std::string(9000, 'x'));
    CHECK(wait_for([&] { return h.overlay->latest().chat.size() == 1; }));
    CHECK(h.overlay->latest().chat[0].text.size() <= proto::kMaxChatChars);
}

TEST(plugin, disconnecting_clears_the_overlays_user_list) {
    Harness h("disconnect");
    h.go_live();
    h.core->on_connect_status_changed(kServer, TsConnectStatus::Disconnected, 0);
    CHECK(wait_for([&] { return h.seen().users.empty(); }));
    CHECK(h.seen().server.connection == ConnectionState::Disconnected);
}

TEST(plugin, a_server_stopping_is_reported_as_a_disconnection) {
    Harness h("serverstop");
    h.go_live();
    h.core->on_server_stopped(kServer);
    CHECK(wait_for([&] {
        return h.seen().server.connection == ConnectionState::Disconnected &&
               h.seen().users.empty();
    }));
}

TEST(plugin, reconnecting_restores_the_roster) {
    Harness h("reconnect");
    h.go_live();
    h.core->on_connect_status_changed(kServer, TsConnectStatus::Disconnected, 0);
    CHECK(wait_for([&] { return h.seen().users.empty(); }));

    h.core->on_connect_status_changed(kServer, TsConnectStatus::Connected, 0);
    CHECK(wait_for([&] { return h.seen().users.size() == 2; }));
    CHECK_EQ(h.seen().channel.name, std::string("Racing #1"));
}

TEST(plugin, intermediate_connecting_states_do_not_each_become_a_notification) {
    Harness h("connect-churn");
    h.connect_overlay();
    CHECK(wait_for([&] { return h.overlay->diagnostics().state == LinkState::Connected; }));
    h.overlay->drain_events();

    h.core->on_connect_status_changed(kServer, TsConnectStatus::Connecting, 0);
    h.core->on_connect_status_changed(kServer, TsConnectStatus::Establishing, 0);
    h.core->on_connect_status_changed(kServer, TsConnectStatus::Connected, 0);
    h.core->on_connect_status_changed(kServer, TsConnectStatus::Established, 0);
    CHECK(wait_for([&] { return h.seen().users.size() == 2; }));
    std::this_thread::sleep_for(200ms);

    std::size_t connection_events = 0;
    for (const OverlayEvent& e : h.overlay->drain_events()) {
        if (e.kind == OverlayEventKind::ConnectionChanged) ++connection_events;
    }
    // Connecting and Establishing collapse to one state, as do Connected and Established.
    CHECK(connection_events <= 2);
}

TEST(plugin, a_subscription_batch_finishing_resynchronises_membership) {
    Harness h("subscribe");
    h.go_live();

    // A client that was already in our channel becomes visible only now.
    h.client.servers[kServer].clients[kBob].channel = kRacing;
    h.core->on_subscription_finished(kServer);
    CHECK(wait_for([&] { return h.seen().users.size() == 3; }));
}

TEST(plugin, a_channel_rename_is_reflected) {
    Harness h("rename");
    h.go_live();
    h.client.servers[kServer].channels[kRacing].name = "Racing #2";
    h.core->on_channel_updated(kServer, kRacing);
    CHECK(wait_for([&] { return h.seen().channel.name == "Racing #2"; }));
}

TEST(plugin, an_unrelated_channel_being_edited_is_ignored) {
    Harness h("rename-other");
    h.go_live();
    h.client.servers[kServer].channels[kLobby].name = "Renamed Lobby";
    h.core->on_channel_updated(kServer, kLobby);
    std::this_thread::sleep_for(150ms);
    CHECK_EQ(h.seen().channel.name, std::string("Racing #1"));
}

TEST(plugin, capabilities_claim_only_what_the_api_provides) {
    const std::vector<std::string> caps = supported_capabilities();
    const auto has = [&](const char* name) {
        return std::find(caps.begin(), caps.end(), name) != caps.end();
    };
    CHECK(has("commander"));
    CHECK(has("whisper_incoming"));
    CHECK(has("speaker_mute_independent"));
    // Things Plugin API 26 does not expose must never be advertised.
    CHECK(!has("whisper_outgoing"));
    CHECK(!has("whisper_targets"));
    CHECK(!has("avatar_images"));
    CHECK(!has("audio_levels"));
}

TEST(plugin, diagnostics_describe_the_live_integration) {
    Harness h("diagnostics");
    h.go_live();
    const std::vector<std::string> lines = h.core->diagnostics();
    CHECK(lines.size() > 5);

    std::string all;
    for (const std::string& line : lines) all += line + "\n";
    CHECK(all.find("Racing #1") != std::string::npos);
    CHECK(all.find("Example TS") != std::string::npos);
    CHECK(all.find("Overlay clients: 1") != std::string::npos);
}
