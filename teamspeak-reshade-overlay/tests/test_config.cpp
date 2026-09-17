// SPDX-License-Identifier: MIT
#include "tsro/config.hpp"
#include "tsro_test.hpp"

using namespace tsro;

TEST(config, defaults_round_trip_exactly) {
    const Config original = Config::defaults();
    ConfigDiagnostics diag;
    const Config back = Config::parse(original.serialise(), diag);

    CHECK(diag.issues.empty());
    CHECK(!diag.from_defaults);
    CHECK_EQ(back.serialise(), original.serialise());
}

TEST(config, the_default_commander_indicator_matches_the_specification) {
    // An orange circular indicator immediately before the name.
    const Config c = Config::defaults();
    CHECK(c.indicators.commander.enabled);
    CHECK(c.indicators.commander.icon == IconShape::Circle);
    CHECK_EQ(c.indicators.commander.icon_color.to_hex(), std::string("#FF952BFF"));
}

TEST(config, microphone_and_speaker_mute_have_distinct_defaults) {
    const Config c = Config::defaults();
    CHECK(c.indicators.mic_muted.icon != c.indicators.speaker_muted.icon);
    CHECK(c.indicators.mic_muted.icon_color != c.indicators.speaker_muted.icon_color);
}

TEST(config, private_messages_are_off_by_default) {
    const Config c = Config::defaults();
    CHECK_EQ(c.chat.show_private_messages, false);
    CHECK_EQ(c.logging.include_message_content, false);
}

TEST(config, malformed_json_yields_defaults_and_an_error) {
    ConfigDiagnostics diag;
    const Config c = Config::parse("{not json", diag);
    CHECK(diag.from_defaults);
    CHECK(!diag.issues.empty());
    CHECK_EQ(c.serialise(), Config::defaults().serialise());
}

TEST(config, a_truncated_file_yields_defaults_rather_than_a_crash) {
    const std::string full = Config::defaults().serialise();
    ConfigDiagnostics diag;
    const Config c = Config::parse(full.substr(0, full.size() / 2), diag);
    CHECK(diag.from_defaults);
    CHECK_EQ(c.appearance.font_size, Config::defaults().appearance.font_size);
}

TEST(config, out_of_range_numbers_are_clamped_and_reported) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"appearance":{"font_size":10000},"general":{"scale":-5}})", diag);
    CHECK(c.appearance.font_size <= 96.0f);
    CHECK(c.general.scale >= 0.25f);
    CHECK(!diag.issues.empty());
}

TEST(config, wrongly_typed_values_keep_the_default) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"general":{"enabled":"yes","master_opacity":"lots"}})", diag);
    CHECK_EQ(c.general.enabled, Config::defaults().general.enabled);
    CHECK_EQ(c.general.master_opacity, Config::defaults().general.master_opacity);
    CHECK(diag.issues.size() >= 2);
}

TEST(config, invalid_colours_keep_the_default_and_warn) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"appearance":{"accent":"not a colour"}})", diag);
    CHECK_EQ(c.appearance.accent, Config::defaults().appearance.accent);
    CHECK(!diag.issues.empty());
}

TEST(config, colour_parsing_accepts_every_documented_form) {
    CHECK_EQ(Color::from_hex("#f80")->to_hex(), std::string("#FF8800FF"));
    CHECK_EQ(Color::from_hex("f80")->to_hex(), std::string("#FF8800FF"));
    CHECK_EQ(Color::from_hex("#FF8800")->to_hex(), std::string("#FF8800FF"));
    CHECK_EQ(Color::from_hex("#FF880080")->to_hex(), std::string("#FF880080"));
    CHECK_EQ(Color::from_hex("#f808")->to_hex(), std::string("#FF880088"));
    CHECK(!Color::from_hex("#12345").has_value());
    CHECK(!Color::from_hex("#gggggg").has_value());
    CHECK(!Color::from_hex("").has_value());
}

TEST(config, colour_packs_for_the_draw_list_in_abgr) {
    const Color c{0x12, 0x34, 0x56, 0x78};
    CHECK_EQ(c.to_abgr(), 0x78563412u);
}

TEST(config, unknown_sections_are_preserved_across_a_round_trip) {
    // A config written by a future version must survive being loaded and saved by this one.
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"future_feature":{"nested":[1,2,3]},"general":{"scale":1.5}})",
        diag);
    CHECK_EQ(c.general.scale, 1.5f);
    const std::string out = c.serialise();
    CHECK(out.find("future_feature") != std::string::npos);
    CHECK(out.find("\"nested\"") != std::string::npos);
}

TEST(config, a_missing_version_is_treated_as_version_one) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(R"({"general":{"scale":2.0}})", diag);
    CHECK(!diag.from_defaults);
    CHECK_EQ(c.general.scale, 2.0f);
    CHECK_EQ(c.config_version, kConfigVersion);
}

TEST(config, a_newer_version_is_flagged_but_still_usable) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":999,"general":{"scale":1.25},"tomorrows_key":true})", diag);
    CHECK(diag.newer_than_supported);
    CHECK(!diag.from_defaults);
    CHECK_EQ(c.general.scale, 1.25f);
    CHECK(c.serialise().find("tomorrows_key") != std::string::npos);
}

TEST(config, per_user_overrides_are_keyed_on_the_teamspeak_identity) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(R"({
        "config_version":1,
        "user_overrides":{
            "kZ9abcDEF/ghi=":{"name_color":"#00FFFF","display_override":"Chief"},
            "other=":{"enabled":false,"name_color":"#FF0000"}
        }})", diag);

    const UserOverride* found = c.find_user_override("kZ9abcDEF/ghi=");
    CHECK(found != nullptr);
    CHECK_EQ(found->name_color->to_hex(), std::string("#00FFFFFF"));
    CHECK_EQ(found->display_override, std::string("Chief"));

    CHECK_EQ(c.find_user_override("other="), nullptr);   // present but disabled
    CHECK_EQ(c.find_user_override("nobody="), nullptr);
    CHECK_EQ(c.find_user_override(""), nullptr);
}

TEST(config, channel_overrides_require_a_server_scoped_key) {
    // A bare channel id would apply one server's theme to a different server's channel.
    ConfigDiagnostics diag;
    const Config c = Config::parse(R"({
        "config_version":1,
        "channel_overrides":{
            "srvUID=:42":{"title_color":"#FF00FF"},
            "42":{"title_color":"#00FF00"}
        }})", diag);

    CHECK_EQ(c.channel_overrides.size(), std::size_t{1});
    CHECK(c.find_channel_override("srvUID=", 42) != nullptr);
    CHECK_EQ(c.find_channel_override("otherUID=", 42), nullptr);
    bool warned = false;
    for (const auto& issue : diag.issues) {
        if (issue.path == "channel_overrides.42") warned = true;
    }
    CHECK(warned);
}

TEST(config, channel_key_construction_is_stable) {
    CHECK_EQ(make_channel_key("srv=", 42), std::string("srv=:42"));
    CHECK_EQ(make_channel_key("", 0), std::string(":0"));
}

TEST(config, overrides_survive_a_round_trip) {
    Config c = Config::defaults();
    UserOverride ov;
    ov.name_color = Color{0, 255, 255, 255};
    ov.icon = IconShape::Star;
    ov.note = "team lead";
    c.user_overrides["kZ9="] = ov;

    ChannelOverride co;
    co.title_color = Color{255, 0, 255, 255};
    co.font_scale = 1.4f;
    c.channel_overrides["srv=:42"] = co;

    ConfigDiagnostics diag;
    const Config back = Config::parse(c.serialise(), diag);
    CHECK_EQ(back.user_overrides.size(), std::size_t{1});
    CHECK(back.find_user_override("kZ9=")->icon.value() == IconShape::Star);
    CHECK_EQ(back.find_user_override("kZ9=")->note, std::string("team lead"));
    CHECK_NEAR(*back.find_channel_override("srv=", 42)->font_scale, 1.4f, 0.001f);
}

TEST(config, every_enum_survives_a_round_trip) {
    Config c = Config::defaults();
    c.user_list.sort = UserSort::TalkPower;
    c.user_list.name_overflow = OverflowMode::Scroll;
    c.channel_title.placement.anchor = Anchor::BottomCenter;
    c.channel_title.placement.align = Align::Center;
    c.notifications.stack = StackDirection::Up;
    c.chat.order = ChatOrder::NewestTop;
    c.animation.speaking = SpeakingAnimation::BorderSweep;
    c.animation.overlay_fade.easing = Easing::EaseOutElastic;
    c.indicators.away.icon = IconShape::Crown;

    ConfigDiagnostics diag;
    const Config back = Config::parse(c.serialise(), diag);
    CHECK(back.user_list.sort == UserSort::TalkPower);
    CHECK(back.user_list.name_overflow == OverflowMode::Scroll);
    CHECK(back.channel_title.placement.anchor == Anchor::BottomCenter);
    CHECK(back.channel_title.placement.align == Align::Center);
    CHECK(back.notifications.stack == StackDirection::Up);
    CHECK(back.chat.order == ChatOrder::NewestTop);
    CHECK(back.animation.speaking == SpeakingAnimation::BorderSweep);
    CHECK(back.animation.overlay_fade.easing == Easing::EaseOutElastic);
    CHECK(back.indicators.away.icon == IconShape::Crown);
}

TEST(config, an_unrecognised_enum_keeps_the_default_and_warns) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"user_list":{"sort":"by_vibes"}})", diag);
    CHECK(c.user_list.sort == Config::defaults().user_list.sort);
    CHECK(!diag.issues.empty());
}

TEST(config, cross_field_constraints_are_repaired) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(R"({
        "config_version":1,
        "chat":{"history_size":5,"max_visible_messages":40},
        "integration":{"reconnect_initial_ms":2000,"reconnect_max_ms":100}
    })", diag);
    CHECK(c.chat.max_visible_messages <= c.chat.history_size);
    CHECK(c.integration.reconnect_max_ms >= c.integration.reconnect_initial_ms);
}

TEST(config, an_unrecognised_log_level_is_reset) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(R"({"config_version":1,"logging":{"level":"loud"}})", diag);
    CHECK_EQ(c.logging.level, std::string("info"));
}

TEST(config, a_section_of_the_wrong_type_falls_back_without_losing_the_rest) {
    ConfigDiagnostics diag;
    const Config c = Config::parse(
        R"({"config_version":1,"appearance":"broken","general":{"scale":1.75}})", diag);
    CHECK_EQ(c.appearance.font_size, Config::defaults().appearance.font_size);
    CHECK_EQ(c.general.scale, 1.75f);
}

TEST(config, easing_curves_are_bounded_at_the_endpoints) {
    for (const Easing e : {Easing::Linear, Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut,
                           Easing::EaseOutBack, Easing::EaseOutElastic}) {
        CHECK_NEAR(ease(e, 0.0f), 0.0f, 0.0001f);
        CHECK_NEAR(ease(e, 1.0f), 1.0f, 0.0001f);
        CHECK_NEAR(ease(e, -5.0f), 0.0f, 0.0001f);
        CHECK_NEAR(ease(e, 5.0f), 1.0f, 0.0001f);
    }
}

TEST(config, alpha_scaling_is_clamped) {
    const Color c{255, 255, 255, 200};
    CHECK_EQ(c.with_alpha_scale(0.5f).a, 100);
    CHECK_EQ(c.with_alpha_scale(-1.0f).a, 0);
    CHECK_EQ(c.with_alpha_scale(9.0f).a, 200);
}
