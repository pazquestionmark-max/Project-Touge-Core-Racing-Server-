// SPDX-License-Identifier: MIT
#include "tsro/protocol.hpp"
#include "tsro/notifications.hpp"
#include "tsro_test.hpp"

using namespace tsro;

namespace {

OverlayEvent join_event(const char* name, std::int64_t ts) {
    OverlayEvent e;
    e.kind = OverlayEventKind::UserJoined;
    e.unique_id = std::string(name) + "=";
    e.display_name = name;
    e.channel_name = "Racing";
    e.previous_channel_name = "Lobby";
    e.timestamp_ms = ts;
    return e;
}

}  // namespace

TEST(notifications, a_join_produces_a_formatted_notification) {
    Config cfg = Config::defaults();
    NotificationQueue queue;
    queue.submit(join_event("Alice", 1000), cfg, 1000);

    CHECK_EQ(queue.size(), std::size_t{1});
    CHECK_EQ(queue.items().front().text, std::string("Alice joined from Lobby"));
    CHECK_EQ(queue.items().front().prefix, std::string("[+]"));
}

TEST(notifications, a_leave_uses_its_own_independent_style) {
    Config cfg = Config::defaults();
    NotificationQueue queue;
    OverlayEvent e = join_event("Bob", 1000);
    e.kind = OverlayEventKind::UserLeft;
    queue.submit(e, cfg, 1000);
    CHECK_EQ(queue.items().front().text, std::string("Bob left to Lobby"));
    CHECK_EQ(queue.items().front().prefix, std::string("[-]"));
    CHECK(queue.items().front().name_color != cfg.notifications.join.name_color);
}

TEST(notifications, a_disabled_category_produces_nothing) {
    Config cfg = Config::defaults();
    cfg.notifications.join.enabled = false;
    NotificationQueue queue;
    queue.submit(join_event("Alice", 1000), cfg, 1000);
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(notifications, the_lifecycle_runs_fade_in_visible_fade_out_removed) {
    Config cfg = Config::defaults();
    cfg.notifications.join.fade = Fade{100, 200, 1000, 0.0f, 1.0f, Easing::Linear};
    NotificationQueue queue;
    queue.submit(join_event("Alice", 0), cfg, 0);

    queue.tick(0, cfg);
    CHECK(queue.items().front().phase == NotificationPhase::FadeIn);
    CHECK_NEAR(queue.items().front().opacity(0), 0.0f, 0.01f);
    CHECK_NEAR(queue.items().front().opacity(50), 0.5f, 0.01f);

    queue.tick(500, cfg);
    CHECK(queue.items().front().phase == NotificationPhase::Visible);
    CHECK_NEAR(queue.items().front().opacity(500), 1.0f, 0.01f);

    queue.tick(1200, cfg);
    CHECK(queue.items().front().phase == NotificationPhase::FadeOut);
    CHECK(queue.items().front().opacity(1200) < 1.0f);
    CHECK(queue.items().front().opacity(1200) > 0.0f);

    queue.tick(1301, cfg);
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(notifications, a_notification_is_never_removed_mid_animation) {
    Config cfg = Config::defaults();
    cfg.notifications.join.fade = Fade{100, 400, 500, 0.0f, 1.0f, Easing::Linear};
    NotificationQueue queue;
    queue.submit(join_event("Alice", 0), cfg, 0);

    // Throughout the entire fade-out window the entry must still be present.
    for (std::int64_t t = 600; t < 1000; t += 25) {
        queue.tick(t, cfg);
        CHECK_EQ(queue.size(), std::size_t{1});
        CHECK(queue.items().front().opacity(t) > 0.0f);
    }
    queue.tick(1001, cfg);
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(notifications, opacity_is_monotonic_within_each_phase) {
    Config cfg = Config::defaults();
    cfg.notifications.join.fade = Fade{200, 200, 400, 0.0f, 1.0f, Easing::Linear};
    NotificationQueue queue;
    queue.submit(join_event("Alice", 0), cfg, 0);
    const Notification& n = queue.items().front();

    float previous = -1.0f;
    for (std::int64_t t = 0; t <= 200; t += 10) {
        const float o = n.opacity(t);
        CHECK(o >= previous - 0.001f);
        previous = o;
    }
    previous = 2.0f;
    for (std::int64_t t = 600; t <= 800; t += 10) {
        const float o = n.opacity(t);
        CHECK(o <= previous + 0.001f);
        previous = o;
    }
}

TEST(notifications, the_visible_count_is_hard_capped) {
    Config cfg = Config::defaults();
    cfg.notifications.max_visible = 3;
    cfg.notifications.merge_duplicates = false;
    NotificationQueue queue;
    for (int i = 0; i < 25; ++i) {
        queue.submit(join_event(("User" + std::to_string(i)).c_str(), i), cfg, i);
    }
    CHECK_EQ(queue.size(), std::size_t{3});
    // The survivors are the newest three.
    CHECK(queue.items().back().text.find("User24") != std::string::npos);
}

TEST(notifications, identical_events_are_merged_rather_than_stacked) {
    Config cfg = Config::defaults();
    cfg.notifications.merge_duplicates = true;
    NotificationQueue queue;
    queue.submit(join_event("Alice", 0), cfg, 0);
    queue.submit(join_event("Alice", 10), cfg, 10);
    queue.submit(join_event("Alice", 20), cfg, 20);
    CHECK_EQ(queue.size(), std::size_t{1});
    CHECK_EQ(queue.items().front().repeat_count, 3);
    CHECK_EQ(queue.items().front().created_ms, 20LL);  // lifecycle restarted
}

TEST(notifications, merging_is_disabled_when_configured) {
    Config cfg = Config::defaults();
    cfg.notifications.merge_duplicates = false;
    NotificationQueue queue;
    queue.submit(join_event("Alice", 0), cfg, 0);
    queue.submit(join_event("Alice", 10), cfg, 10);
    CHECK_EQ(queue.size(), std::size_t{2});
}

TEST(notifications, joins_during_initial_synchronisation_are_suppressed) {
    // Reconnecting to a busy channel must not produce a notification per member.
    Config cfg = Config::defaults();
    cfg.notifications.suppress_after_connect_ms = 2500;
    NotificationQueue queue;
    queue.note_connected(1000);

    for (int i = 0; i < 8; ++i) {
        queue.submit(join_event(("User" + std::to_string(i)).c_str(), 1100), cfg, 1100);
    }
    CHECK_EQ(queue.size(), std::size_t{0});

    queue.submit(join_event("LateArrival", 5000), cfg, 5000);
    CHECK_EQ(queue.size(), std::size_t{1});
}

TEST(notifications, a_connection_event_starts_the_suppression_window) {
    Config cfg = Config::defaults();
    NotificationQueue queue;
    OverlayEvent connected;
    connected.kind = OverlayEventKind::ConnectionChanged;
    connected.connection = ConnectionState::Connected;
    connected.timestamp_ms = 1000;
    queue.submit(connected, cfg, 1000);
    CHECK_EQ(queue.size(), std::size_t{1});
    CHECK(queue.items().front().text.find("connected") != std::string::npos);

    queue.submit(join_event("Alice", 1200), cfg, 1200);
    CHECK_EQ(queue.size(), std::size_t{1});  // the join was absorbed
}

TEST(notifications, a_connection_loss_is_worded_distinctly) {
    Config cfg = Config::defaults();
    NotificationQueue queue;
    OverlayEvent lost;
    lost.kind = OverlayEventKind::ConnectionChanged;
    lost.connection = ConnectionState::Disconnected;
    lost.reason = DisconnectReason::ConnectionLost;
    queue.submit(lost, cfg, 1000);
    CHECK(queue.items().front().text.find("connection lost") != std::string::npos);
}

TEST(notifications, a_channel_switch_reports_both_channels) {
    Config cfg = Config::defaults();
    NotificationQueue queue;
    OverlayEvent e;
    e.kind = OverlayEventKind::ChannelChanged;
    e.channel_name = "Racing";
    e.previous_channel_name = "Lobby";
    e.user_count = 4;
    queue.submit(e, cfg, 1000);
    CHECK_EQ(queue.items().front().text, std::string("Lobby -> Racing (4)"));
}

TEST(notifications, indicator_only_events_produce_no_notification) {
    Config cfg = Config::defaults();
    NotificationQueue queue;
    for (const OverlayEventKind kind :
         {OverlayEventKind::SpeakingStarted, OverlayEventKind::SpeakingStopped,
          OverlayEventKind::CommanderChanged, OverlayEventKind::Desynchronised}) {
        OverlayEvent e;
        e.kind = kind;
        queue.submit(e, cfg, 1000);
    }
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(notifications, a_per_user_colour_override_reaches_the_notification) {
    Config cfg = Config::defaults();
    UserOverride ov;
    ov.name_color = Color{0, 255, 255, 255};
    cfg.user_overrides["Alice="] = ov;
    NotificationQueue queue;
    queue.submit(join_event("Alice", 1000), cfg, 1000);
    CHECK_EQ(queue.items().front().name_color.to_hex(), std::string("#00FFFFFF"));
}

TEST(notifications, sounds_are_queued_for_the_renderer_not_played_here) {
    Config cfg = Config::defaults();
    cfg.notifications.join.sound = true;
    cfg.notifications.join.sound_file = "join.wav";
    NotificationQueue queue;
    queue.submit(join_event("Alice", 1000), cfg, 1000);

    const std::vector<std::string> sounds = queue.drain_sounds();
    CHECK_EQ(sounds.size(), std::size_t{1});
    CHECK_EQ(sounds[0], std::string("join.wav"));
    CHECK_EQ(queue.drain_sounds().size(), std::size_t{0});
}

TEST(notifications, a_sound_without_a_file_is_not_queued) {
    Config cfg = Config::defaults();
    cfg.notifications.join.sound = true;
    cfg.notifications.join.sound_file = "";
    NotificationQueue queue;
    queue.submit(join_event("Alice", 1000), cfg, 1000);
    CHECK_EQ(queue.drain_sounds().size(), std::size_t{0});
}

TEST(notifications, zero_animation_durations_mean_instant_on_and_off) {
    Config cfg = Config::defaults();
    cfg.notifications.join.fade = Fade{0, 0, 500, 0.0f, 1.0f, Easing::Linear};
    NotificationQueue queue;
    queue.submit(join_event("Alice", 0), cfg, 0);
    CHECK_NEAR(queue.items().front().opacity(0), 1.0f, 0.01f);
    CHECK_NEAR(queue.items().front().opacity(499), 1.0f, 0.01f);
    queue.tick(501, cfg);
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(notifications, an_all_zero_fade_neither_divides_by_zero_nor_lingers) {
    Config cfg = Config::defaults();
    cfg.notifications.join.fade = Fade{0, 0, 0, 0.0f, 1.0f, Easing::Linear};
    NotificationQueue queue;
    queue.submit(join_event("Alice", 0), cfg, 0);
    CHECK_EQ(queue.items().front().lifetime_ms(), 0LL);
    CHECK(queue.items().front().finished(0));
    queue.tick(0, cfg);
    CHECK_EQ(queue.size(), std::size_t{0});
}

TEST(envelope, speaking_ramps_up_and_decays) {
    SpeakingEnvelope envelope;
    envelope.set_config(100, 200);

    envelope.update({{"a=", true}}, 50);
    const float half = envelope.intensity("a=");
    CHECK(half > 0.0f);
    CHECK(half < 1.0f);

    envelope.update({{"a=", true}}, 100);
    CHECK_NEAR(envelope.intensity("a="), 1.0f, 0.001f);

    envelope.update({{"a=", false}}, 100);
    CHECK(envelope.intensity("a=") < 1.0f);
    envelope.update({{"a=", false}}, 200);
    CHECK_NEAR(envelope.intensity("a="), 0.0f, 0.001f);
}

TEST(envelope, intensity_never_leaves_the_unit_range) {
    SpeakingEnvelope envelope;
    envelope.set_config(50, 50);
    for (int i = 0; i < 100; ++i) {
        envelope.update({{"a=", i % 3 == 0}}, 40);
        const float v = envelope.intensity("a=");
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
    }
}

TEST(envelope, departed_users_are_eventually_forgotten) {
    // Guards against unbounded growth over a long session in a busy channel.
    SpeakingEnvelope envelope;
    envelope.set_config(50, 50);
    for (int i = 0; i < 200; ++i) {
        envelope.update({{"user" + std::to_string(i) + "=", true}}, 100);
    }
    for (int i = 0; i < 40; ++i) {
        envelope.update({{"survivor=", true}}, 100);
    }
    CHECK(envelope.tracked() <= 2);
    CHECK_NEAR(envelope.intensity("survivor="), 1.0f, 0.001f);
}

TEST(envelope, an_unknown_user_has_zero_intensity) {
    SpeakingEnvelope envelope;
    CHECK_NEAR(envelope.intensity("nobody="), 0.0f, 0.0001f);
}

TEST(envelope, zero_durations_are_treated_as_instant) {
    SpeakingEnvelope envelope;
    envelope.set_config(0, 0);
    envelope.update({{"a=", true}}, 16);
    CHECK_NEAR(envelope.intensity("a="), 1.0f, 0.001f);
}

TEST(notifications, a_private_message_names_its_sender) {
    Config cfg = Config::defaults();
    cfg.notifications.private_chat.enabled = true;
    NotificationQueue q;

    OverlayEvent e;
    e.kind = OverlayEventKind::ChatMessage;
    e.chat.category = ChatCategory::Private;
    e.chat.sender_name = "Bob";
    e.chat.text = "need you on channel 2";
    q.submit(e, cfg, 1000);

    CHECK_EQ(q.items().size(), std::size_t{1});
    const Notification& n = q.items().front();
    CHECK(n.kind == NotificationKind::PrivateChat);
    CHECK_EQ(n.name, std::string("Bob"));
    CHECK(n.text.find("Bob") != std::string::npos);
    CHECK(n.text.find("need you on channel 2") != std::string::npos);
}

TEST(notifications, a_channel_message_is_a_different_toast_from_a_private_one) {
    Config cfg = Config::defaults();
    cfg.notifications.chat.enabled = true;
    NotificationQueue q;

    OverlayEvent channel;
    channel.kind = OverlayEventKind::ChatMessage;
    channel.chat.category = ChatCategory::Channel;
    channel.chat.sender_name = "Bob";
    channel.chat.text = "hello";
    q.submit(channel, cfg, 1000);

    CHECK_EQ(q.items().size(), std::size_t{1});
    CHECK(q.items().front().kind == NotificationKind::Chat);
    CHECK(q.items().front().prefix != cfg.notifications.private_chat.prefix);
}

TEST(notifications, an_unnamed_sender_still_produces_something_readable) {
    Config cfg = Config::defaults();
    cfg.notifications.private_chat.enabled = true;
    NotificationQueue q;

    OverlayEvent e;
    e.kind = OverlayEventKind::ChatMessage;
    e.chat.category = ChatCategory::Private;
    e.chat.text = "hello";
    q.submit(e, cfg, 1000);

    CHECK_EQ(q.items().size(), std::size_t{1});
    CHECK(!q.items().front().name.empty());
}

TEST(notifications, private_message_toasts_are_off_until_asked_for) {
    // The brief is explicit that private chat is never exposed by default. Enabling the toast
    // is what also makes the add-on ask the plugin for private messages at all.
    Config cfg = Config::defaults();
    NotificationQueue q;

    OverlayEvent e;
    e.kind = OverlayEventKind::ChatMessage;
    e.chat.category = ChatCategory::Private;
    e.chat.sender_name = "Bob";
    e.chat.text = "hello";
    q.submit(e, cfg, 1000);

    CHECK(q.items().empty());
}

TEST(notifications, an_event_line_widens_while_a_message_wraps) {
    // "X left Y" is one short sentence: it should take the room it needs rather than lose its
    // ending to an ellipsis. Someone else's prose has no length limit, so it wraps instead.
    const Config cfg = Config::defaults();
    CHECK(!cfg.notifications.join.wrap);
    CHECK(!cfg.notifications.leave.wrap);
    CHECK(!cfg.notifications.channel_switch.wrap);
    CHECK(!cfg.notifications.connection.wrap);
    CHECK(cfg.notifications.chat.wrap);
    CHECK(cfg.notifications.private_chat.wrap);
}

TEST(notifications, the_wrap_choice_reaches_the_renderer) {
    Config cfg = Config::defaults();
    cfg.notifications.private_chat.enabled = true;
    NotificationQueue q;

    OverlayEvent joined;
    joined.kind = OverlayEventKind::UserJoined;
    joined.display_name = "Alice";
    joined.channel_name = "Racing";
    q.submit(joined, cfg, 1000);

    OverlayEvent pm;
    pm.kind = OverlayEventKind::ChatMessage;
    pm.chat.category = ChatCategory::Private;
    pm.chat.sender_name = "Bob";
    pm.chat.text = "a much longer message that would widen a toast past anything readable";
    q.submit(pm, cfg, 1000);

    CHECK_EQ(q.items().size(), std::size_t{2});
    CHECK(!q.items().front().wrap);
    CHECK(q.items().back().wrap);
    CHECK(q.items().back().max_lines >= 1);
}

TEST(notifications, a_poke_gets_its_own_toast_naming_the_sender) {
    Config cfg = Config::defaults();
    NotificationQueue q;

    OverlayEvent e;
    e.kind = OverlayEventKind::ChatMessage;
    e.chat.category = ChatCategory::Poke;
    e.chat.sender_name = "Carol";
    e.chat.text = "get in here";
    q.submit(e, cfg, 1000);

    CHECK_EQ(q.items().size(), std::size_t{1});
    const Notification& n = q.items().front();
    CHECK(n.kind == NotificationKind::Poke);
    CHECK_EQ(n.prefix, std::string("[POKE]"));
    CHECK_EQ(n.name, std::string("Carol"));
    CHECK(n.text.find("Carol") != std::string::npos);
    CHECK(n.text.find("get in here") != std::string::npos);
}

TEST(notifications, a_poke_rides_the_private_message_subscription) {
    // Turning private messages on is all it takes: a poke is just as personal, so it is gated
    // by the same switch rather than needing one of its own.
    proto::ConfigurationUpdatedPayload subscription;
    subscription.chat.priv = false;
    CHECK(!subscription.chat.enabled_for(ChatCategory::Poke));
    subscription.chat.priv = true;
    CHECK(subscription.chat.enabled_for(ChatCategory::Poke));
    CHECK(subscription.chat.enabled_for(ChatCategory::Private));
    CHECK(!subscription.chat.enabled_for(ChatCategory::Channel));
}

TEST(notifications, a_join_names_the_channel_they_came_from) {
    Config cfg = Config::defaults();
    NotificationQueue q;
    OverlayEvent e = join_event("Test", 1000);
    e.channel_name = "123";
    e.previous_channel_name = "321";
    q.submit(e, cfg, 1000);
    CHECK_EQ(q.items().front().text, std::string("Test joined from 321"));
}

TEST(notifications, an_unseen_channel_still_reads_as_a_sentence) {
    // Someone can arrive from a channel we are not subscribed to, or join the server outright.
    // The message must not end up with a hole where the channel should be.
    Config cfg = Config::defaults();
    NotificationQueue q;

    OverlayEvent moved = join_event("Test", 1000);
    moved.previous_channel_name.clear();
    moved.cause = JoinCause::Moved;
    q.submit(moved, cfg, 1000);
    CHECK_EQ(q.items().front().text, std::string("Test joined from elsewhere"));

    NotificationQueue q2;
    OverlayEvent arrived = join_event("Test", 1000);
    arrived.previous_channel_name.clear();
    arrived.cause = JoinCause::Connected;
    q2.submit(arrived, cfg, 1000);
    CHECK_EQ(q2.items().front().text, std::string("Test joined from the server"));
}

TEST(notifications, each_placeholder_carries_its_own_colour) {
    Config cfg = Config::defaults();
    cfg.notifications.join.name_color = Color{1, 2, 3, 255};
    cfg.notifications.join.previous_color = Color{4, 5, 6, 255};
    NotificationQueue q;

    OverlayEvent e = join_event("Test", 1000);
    e.previous_channel_name = "321";
    q.submit(e, cfg, 1000);

    const Notification& n = q.items().front();
    CHECK_EQ(n.highlights.size(), std::size_t{2});
    CHECK_EQ(n.text.substr(n.highlights[0].begin, n.highlights[0].end - n.highlights[0].begin),
             std::string("Test"));
    CHECK_EQ(n.highlights[0].color.to_hex(), std::string("#010203FF"));
    CHECK_EQ(n.text.substr(n.highlights[1].begin, n.highlights[1].end - n.highlights[1].begin),
             std::string("321"));
    CHECK_EQ(n.highlights[1].color.to_hex(), std::string("#040506FF"));
}

TEST(notifications, placeholder_colouring_can_be_switched_off) {
    Config cfg = Config::defaults();
    cfg.notifications.join.color_placeholders = false;
    NotificationQueue q;
    q.submit(join_event("Test", 1000), cfg, 1000);
    CHECK(q.items().front().highlights.empty());
}

TEST(notifications, a_whisper_from_outside_your_channel_is_announced) {
    // The bug: whispering across channels is the point of whispering, so the whisperer is
    // usually not in the roster -- and the handler gave up on exactly that case, dropping
    // nearly every whisper there was.
    Config cfg = Config::defaults();
    NotificationQueue q;

    OverlayEvent e;
    e.kind = OverlayEventKind::WhisperStarted;
    e.display_name = "Carol";
    e.from_channel = false;
    q.submit(e, cfg, 1000);

    CHECK_EQ(q.items().size(), std::size_t{1});
    CHECK(q.items().front().kind == NotificationKind::Whisper);
    CHECK(q.items().front().text.find("Carol") != std::string::npos);
}

TEST(notifications, whispers_can_be_filtered_by_where_they_came_from) {
    Config cfg = Config::defaults();
    cfg.notifications.whisper_from_elsewhere = false;
    NotificationQueue q;

    OverlayEvent outside;
    outside.kind = OverlayEventKind::WhisperStarted;
    outside.display_name = "Carol";
    outside.from_channel = false;
    q.submit(outside, cfg, 1000);
    CHECK(q.items().empty());

    OverlayEvent inside;
    inside.kind = OverlayEventKind::WhisperStarted;
    inside.display_name = "Dave";
    inside.from_channel = true;
    q.submit(inside, cfg, 1000);
    CHECK_EQ(q.items().size(), std::size_t{1});
}
