// SPDX-License-Identifier: MIT
#include "tsro/layout.hpp"
#include "tsro_test.hpp"

using namespace tsro;

namespace {

/// Deterministic monospace-like metric: every code point is 0.5 em wide. Keeps the geometry
/// assertions exact and independent of any font.
MeasureFn stub_measure() {
    return [](std::string_view text, float font_size) {
        std::size_t glyphs = 0;
        for (char c : text) {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++glyphs;
        }
        return static_cast<float>(glyphs) * font_size * 0.5f;
    };
}

UserState make_user(const char* uid, const char* name) {
    UserState u;
    u.unique_id = uid;
    u.nickname = name;
    u.display_name = name;
    return u;
}

OverlayState connected_state(std::vector<UserState> users) {
    OverlayState s;
    s.server.connection = ConnectionState::Connected;
    s.server.unique_id = "srv=";
    s.server.name = "Example";
    s.channel.id = 42;
    s.channel.name = "Racing";
    s.channel.parent_name = "Games";
    s.synchronised = true;
    s.users = std::move(users);
    return s;
}

}  // namespace

TEST(layout, anchors_place_elements_against_the_right_edges) {
    const Viewport vp{1920.0f, 1080.0f};
    Placement p;
    p.x = 24.0f;
    p.y = 16.0f;

    p.anchor = Anchor::TopLeft;
    Rect r = resolve_placement(p, 200.0f, 50.0f, vp);
    CHECK_NEAR(r.x, 24.0f, 0.01f);
    CHECK_NEAR(r.y, 16.0f, 0.01f);

    p.anchor = Anchor::BottomRight;
    r = resolve_placement(p, 200.0f, 50.0f, vp);
    CHECK_NEAR(r.right(), 1920.0f - 24.0f, 0.01f);
    CHECK_NEAR(r.bottom(), 1080.0f - 16.0f, 0.01f);

    p.anchor = Anchor::Center;
    p.x = 0.0f;
    p.y = 0.0f;
    r = resolve_placement(p, 200.0f, 50.0f, vp);
    CHECK_NEAR(r.x + r.w * 0.5f, 960.0f, 0.01f);
    CHECK_NEAR(r.y + r.h * 0.5f, 540.0f, 0.01f);

    p.anchor = Anchor::TopCenter;
    r = resolve_placement(p, 200.0f, 50.0f, vp);
    CHECK_NEAR(r.x + r.w * 0.5f, 960.0f, 0.01f);
    CHECK_NEAR(r.y, 0.0f, 0.01f);
}

TEST(layout, all_nine_anchors_stay_inside_the_viewport) {
    const Viewport vp{1280.0f, 720.0f};
    for (const Anchor a : {Anchor::TopLeft, Anchor::TopCenter, Anchor::TopRight,
                           Anchor::CenterLeft, Anchor::Center, Anchor::CenterRight,
                           Anchor::BottomLeft, Anchor::BottomCenter, Anchor::BottomRight}) {
        Placement p;
        p.anchor = a;
        p.x = 10.0f;
        p.y = 10.0f;
        const Rect r = resolve_placement(p, 100.0f, 40.0f, vp);
        CHECK(r.x >= -0.01f);
        CHECK(r.y >= -0.01f);
        CHECK(r.right() <= vp.width + 0.01f);
        CHECK(r.bottom() <= vp.height + 0.01f);
    }
}

TEST(layout, a_bottom_right_element_tracks_the_corner_across_resolutions) {
    // The property that makes the overlay resolution-independent.
    Placement p;
    p.anchor = Anchor::BottomRight;
    p.x = 32.0f;
    p.y = 32.0f;
    for (const Viewport vp : {Viewport{1280.0f, 720.0f}, Viewport{1920.0f, 1080.0f},
                              Viewport{2560.0f, 1080.0f}, Viewport{3840.0f, 2160.0f},
                              Viewport{1024.0f, 768.0f}, Viewport{5120.0f, 1440.0f}}) {
        const Rect r = resolve_placement(p, 300.0f, 120.0f, vp);
        CHECK_NEAR(vp.width - r.right(), 32.0f, 0.01f);
        CHECK_NEAR(vp.height - r.bottom(), 32.0f, 0.01f);
    }
}

TEST(layout, percentage_offsets_scale_with_the_viewport) {
    Placement p;
    p.anchor = Anchor::TopLeft;
    p.percent = true;
    p.x = 0.25f;
    p.y = 0.5f;

    Rect a = resolve_placement(p, 100.0f, 50.0f, Viewport{1920.0f, 1080.0f});
    CHECK_NEAR(a.x, 480.0f, 0.01f);
    CHECK_NEAR(a.y, 540.0f, 0.01f);

    Rect b = resolve_placement(p, 100.0f, 50.0f, Viewport{1280.0f, 720.0f});
    CHECK_NEAR(b.x, 320.0f, 0.01f);
    CHECK_NEAR(b.y, 360.0f, 0.01f);
}

TEST(layout, alignment_offsets_are_correct) {
    CHECK_NEAR(align_offset(Align::Left, 200.0f, 50.0f), 0.0f, 0.01f);
    CHECK_NEAR(align_offset(Align::Center, 200.0f, 50.0f), 75.0f, 0.01f);
    CHECK_NEAR(align_offset(Align::Right, 200.0f, 50.0f), 150.0f, 0.01f);
}

TEST(layout, short_text_is_left_untouched) {
    const auto measure = stub_measure();
    const FittedText f = fit_text("Alice", 500.0f, 16.0f, OverflowMode::Ellipsis, 0.7f, measure);
    CHECK_EQ(f.text, std::string("Alice"));
    CHECK(!f.truncated);
    CHECK_NEAR(f.font_scale, 1.0f, 0.001f);
}

TEST(layout, ellipsis_overflow_fits_within_the_budget) {
    const auto measure = stub_measure();
    const std::string name = "AVeryLongTeamSpeakNicknameIndeed";
    const FittedText f = fit_text(name, 80.0f, 16.0f, OverflowMode::Ellipsis, 0.7f, measure);
    CHECK(f.truncated);
    CHECK(f.width <= 80.0f + 0.01f);
    CHECK(f.text.size() < name.size() + 3);
    CHECK(f.text.find("\xE2\x80\xA6") != std::string::npos);
}

TEST(layout, clip_overflow_fits_within_the_budget) {
    const auto measure = stub_measure();
    const FittedText f =
        fit_text("AVeryLongTeamSpeakNickname", 80.0f, 16.0f, OverflowMode::Clip, 0.7f, measure);
    CHECK(f.truncated);
    CHECK(measure(f.text, 16.0f) <= 80.0f + 0.01f);
}

TEST(layout, shrink_overflow_respects_the_font_floor) {
    const auto measure = stub_measure();
    const FittedText f = fit_text("AVeryLongTeamSpeakNicknameIndeedYesReally", 80.0f, 16.0f,
                                  OverflowMode::Shrink, 0.75f, measure);
    CHECK(f.font_scale >= 0.75f - 0.001f);
    CHECK(f.font_scale <= 1.0f);
}

TEST(layout, wrap_overflow_produces_bounded_lines) {
    const auto measure = stub_measure();
    const FittedText f = fit_text("one two three four five six seven eight", 60.0f, 16.0f,
                                  OverflowMode::Wrap, 0.7f, measure);
    CHECK(f.lines.size() > 1);
    CHECK(f.lines.size() <= 8);  // hard cap: one name may never own the whole panel
    for (const std::string& line : f.lines) {
        CHECK(!line.empty());
    }
}

TEST(layout, wrap_makes_progress_on_a_single_oversized_glyph_run) {
    const auto measure = stub_measure();
    const FittedText f =
        fit_text("AAAAAAAAAAAAAAAAAAAA", 4.0f, 16.0f, OverflowMode::Wrap, 0.7f, measure);
    CHECK(!f.lines.empty());  // must terminate rather than loop forever
}

TEST(layout, scroll_overflow_stays_within_the_overhang) {
    const auto measure = stub_measure();
    const std::string name = "AVeryLongTeamSpeakNickname";
    const float full = measure(name, 16.0f);
    const float box = 60.0f;
    for (float t = 0.0f; t < 30.0f; t += 0.37f) {
        const FittedText f =
            fit_text(name, box, 16.0f, OverflowMode::Scroll, 0.7f, measure, t);
        CHECK(f.scroll_offset >= -0.01f);
        CHECK(f.scroll_offset <= full - box + 0.01f);
    }
}

TEST(layout, truncation_never_splits_a_utf8_sequence) {
    const auto measure = stub_measure();
    const std::string name = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    for (const OverflowMode mode :
         {OverflowMode::Clip, OverflowMode::Ellipsis, OverflowMode::Wrap}) {
        const FittedText f = fit_text(name, 30.0f, 16.0f, mode, 0.7f, measure);
        CHECK(json::is_valid_utf8(f.text));
        for (const std::string& line : f.lines) {
            CHECK(json::is_valid_utf8(line));
        }
    }
}

TEST(layout, the_commander_indicator_leads_the_name) {
    Config cfg = Config::defaults();
    UserState u = make_user("a=", "Alice");
    u.channel_commander = true;
    const ResolvedUser r = resolve_user(u, cfg, 0.0f);
    CHECK_EQ(r.leading.size(), std::size_t{1});
    CHECK(r.leading[0].shape == IconShape::Circle);
    CHECK_EQ(r.leading[0].color.to_hex(), std::string("#FF952BFF"));
}

TEST(layout, a_per_user_commander_colour_overrides_the_default) {
    Config cfg = Config::defaults();
    UserOverride ov;
    ov.commander_color = Color{0, 255, 255, 255};
    cfg.user_overrides["a="] = ov;

    UserState u = make_user("a=", "Alice");
    u.channel_commander = true;
    const ResolvedUser r = resolve_user(u, cfg, 0.0f);
    CHECK_EQ(r.leading[0].color.to_hex(), std::string("#00FFFFFF"));
}

TEST(layout, per_user_display_and_name_colours_are_applied) {
    Config cfg = Config::defaults();
    UserOverride ov;
    ov.name_color = Color{128, 0, 255, 255};
    ov.display_override = "Chief";
    cfg.user_overrides["a="] = ov;

    const ResolvedUser r = resolve_user(make_user("a=", "Alice"), cfg, 0.0f);
    CHECK_EQ(r.display, std::string("Chief"));
    CHECK_EQ(r.name_color.to_hex(), std::string("#8000FFFF"));
}

TEST(layout, microphone_and_speaker_mute_produce_separate_indicators) {
    Config cfg = Config::defaults();
    UserState u = make_user("a=", "Alice");
    u.input_muted = true;
    u.output_muted = true;
    const ResolvedUser r = resolve_user(u, cfg, 0.0f);

    bool mic = false, speaker = false;
    for (const auto& i : r.trailing) {
        if (i.shape == cfg.indicators.mic_muted.icon) mic = true;
        if (i.shape == cfg.indicators.speaker_muted.icon) speaker = true;
    }
    CHECK(mic);
    CHECK(speaker);
}

TEST(layout, speaker_mute_alone_does_not_show_a_microphone_indicator) {
    Config cfg = Config::defaults();
    UserState u = make_user("a=", "Alice");
    u.output_muted = true;
    const ResolvedUser r = resolve_user(u, cfg, 0.0f);
    for (const auto& i : r.trailing) {
        CHECK(i.shape != cfg.indicators.mic_muted.icon);
    }
}

TEST(layout, an_unknown_state_renders_no_indicator) {
    // Absent must mean "hide", never "false".
    Config cfg = Config::defaults();
    const ResolvedUser r = resolve_user(make_user("a=", "Alice"), cfg, 0.0f);
    CHECK_EQ(r.trailing.size(), std::size_t{0});
    CHECK_EQ(r.leading.size(), std::size_t{0});
}

TEST(layout, a_disabled_indicator_is_omitted) {
    Config cfg = Config::defaults();
    cfg.indicators.commander.enabled = false;
    UserState u = make_user("a=", "Alice");
    u.channel_commander = true;
    CHECK_EQ(resolve_user(u, cfg, 0.0f).leading.size(), std::size_t{0});
}

TEST(layout, the_speaking_envelope_blends_the_colour) {
    Config cfg = Config::defaults();
    UserState u = make_user("a=", "Alice");
    u.talking = true;
    const Color cold = resolve_user(u, cfg, 0.0f).name_color;
    const Color warm = resolve_user(u, cfg, 1.0f).name_color;
    CHECK(cold != warm);
    CHECK_EQ(warm.to_hex(), cfg.indicators.speaking.text_color.to_hex());
}

TEST(layout, the_local_user_is_distinguished) {
    Config cfg = Config::defaults();
    UserState u = make_user("me=", "Me");
    u.is_self = true;
    CHECK_EQ(resolve_user(u, cfg, 0.0f).name_color.to_hex(),
             cfg.user_list.local_user_color.to_hex());
}

TEST(layout, hiding_the_local_user_removes_them_from_the_list) {
    Config cfg = Config::defaults();
    cfg.user_list.show_local_user = false;
    UserState me = make_user("me=", "Me");
    me.is_self = true;
    const OverlayState s = connected_state({me, make_user("a=", "Alice")});
    CHECK_EQ(order_users(s, cfg).size(), std::size_t{1});
}

TEST(layout, hiding_muted_users_keeps_the_local_user) {
    Config cfg = Config::defaults();
    cfg.user_list.show_muted_users = false;
    UserState me = make_user("me=", "Me");
    me.is_self = true;
    me.input_muted = true;
    UserState other = make_user("a=", "Alice");
    other.input_muted = true;

    const std::vector<const UserState*> ordered =
        order_users(connected_state({me, other}), cfg);
    CHECK_EQ(ordered.size(), std::size_t{1});
    CHECK(ordered[0]->is_self);
}

TEST(layout, alphabetical_sorting_is_case_insensitive_and_stable) {
    Config cfg = Config::defaults();
    cfg.user_list.sort = UserSort::Alphabetical;
    const OverlayState s = connected_state(
        {make_user("c=", "charlie"), make_user("a=", "Alice"), make_user("b=", "bob")});
    const std::vector<const UserState*> ordered = order_users(s, cfg);
    CHECK_EQ(ordered[0]->nickname, std::string("Alice"));
    CHECK_EQ(ordered[1]->nickname, std::string("bob"));
    CHECK_EQ(ordered[2]->nickname, std::string("charlie"));
}

TEST(layout, speaking_first_lifts_talkers_to_the_top) {
    Config cfg = Config::defaults();
    cfg.user_list.speaking_first = true;
    UserState zoe = make_user("z=", "Zoe");
    zoe.talking = true;
    const OverlayState s = connected_state({make_user("a=", "Alice"), zoe});
    CHECK_EQ(order_users(s, cfg)[0]->nickname, std::string("Zoe"));
}

TEST(layout, talk_power_sorting_is_descending) {
    Config cfg = Config::defaults();
    cfg.user_list.sort = UserSort::TalkPower;
    UserState low = make_user("a=", "Low");
    low.talk_power = 10;
    UserState high = make_user("b=", "High");
    high.talk_power = 100;
    const std::vector<const UserState*> ordered =
        order_users(connected_state({low, high}), cfg);
    CHECK_EQ(ordered[0]->nickname, std::string("High"));
}

TEST(layout, the_user_list_is_capped_and_reports_the_remainder) {
    Config cfg = Config::defaults();
    cfg.user_list.max_visible_users = 3;
    std::vector<UserState> users;
    for (int i = 0; i < 10; ++i) {
        users.push_back(make_user(("u" + std::to_string(i) + "=").c_str(),
                                  ("User" + std::to_string(i)).c_str()));
    }
    const LayoutResult r = compute_layout(connected_state(users), cfg, Viewport{1920, 1080},
                                          stub_measure(), 0.0f, nullptr);
    CHECK_EQ(r.users.rows.size(), std::size_t{3});
    CHECK_EQ(r.users.hidden_count, 7);
    CHECK_EQ(r.users.overflow_text, std::string("+7 more"));
}

TEST(layout, rows_are_laid_out_inside_the_list_rectangle) {
    Config cfg = Config::defaults();
    const LayoutResult r =
        compute_layout(connected_state({make_user("a=", "Alice"), make_user("b=", "Bob")}), cfg,
                       Viewport{1920, 1080}, stub_measure(), 0.0f, nullptr);
    CHECK_EQ(r.users.rows.size(), std::size_t{2});
    for (const auto& row : r.users.rows) {
        CHECK(row.rect.y >= r.users.rect.y - 0.01f);
        CHECK(row.rect.bottom() <= r.users.rect.bottom() + 0.01f);
        CHECK(row.rect.x >= r.users.rect.x - 0.01f);
    }
    CHECK(r.users.rows[1].rect.y > r.users.rows[0].rect.y);
}

TEST(layout, the_title_shows_the_parent_channel_when_configured) {
    Config cfg = Config::defaults();
    cfg.channel_title.show_parent = true;
    cfg.channel_title.show_user_count = false;
    const LayoutResult r = compute_layout(connected_state({}), cfg, Viewport{1920, 1080},
                                          stub_measure(), 0.0f, nullptr);
    CHECK(r.title.visible);
    CHECK_EQ(r.title.text, std::string("Games / Racing"));
}

TEST(layout, the_title_shows_the_user_count_when_configured) {
    Config cfg = Config::defaults();
    cfg.channel_title.show_parent = false;
    cfg.channel_title.show_user_count = true;
    const LayoutResult r =
        compute_layout(connected_state({make_user("a=", "Alice"), make_user("b=", "Bob")}), cfg,
                       Viewport{1920, 1080}, stub_measure(), 0.0f, nullptr);
    CHECK_EQ(r.title.text, std::string("Racing (2)"));
}

TEST(layout, a_disconnected_state_is_reported_as_degraded) {
    Config cfg = Config::defaults();
    OverlayState s;
    s.server.connection = ConnectionState::Disconnected;
    const LayoutResult r =
        compute_layout(s, cfg, Viewport{1920, 1080}, stub_measure(), 0.0f, nullptr);
    CHECK(r.degraded);
    CHECK(!r.users.visible);
    CHECK_EQ(r.title.text, cfg.channel_title.disconnected_text);
}

TEST(layout, a_stale_state_is_reported_as_degraded) {
    Config cfg = Config::defaults();
    OverlayState s = connected_state({make_user("a=", "Alice")});
    s.stale = true;
    const LayoutResult r =
        compute_layout(s, cfg, Viewport{1920, 1080}, stub_measure(), 0.0f, nullptr);
    CHECK(r.degraded);
    CHECK(!r.degraded_reason.empty());
}

TEST(layout, a_channel_override_restyles_the_title) {
    Config cfg = Config::defaults();
    ChannelOverride co;
    co.title_color = Color{255, 0, 255, 255};
    co.display_override = "Home";
    cfg.channel_overrides["srv=:42"] = co;
    cfg.channel_title.show_parent = false;
    cfg.channel_title.show_user_count = false;

    const LayoutResult r = compute_layout(connected_state({}), cfg, Viewport{1920, 1080},
                                          stub_measure(), 0.0f, nullptr);
    CHECK_EQ(r.title.text, std::string("Home"));
    CHECK_EQ(r.title.text_color.to_hex(), std::string("#FF00FFFF"));
}

TEST(layout, format_templates_substitute_known_placeholders) {
    FormatValues v;
    v.name = "Alice";
    v.channel = "Racing";
    v.previous = "Lobby";
    v.count = "4";
    CHECK_EQ(format_template("{name} joined {channel}", v), std::string("Alice joined Racing"));
    CHECK_EQ(format_template("{previous} -> {channel} ({count})", v),
             std::string("Lobby -> Racing (4)"));
}

TEST(layout, the_parent_placeholder_is_distinct_from_previous) {
    // {parent} is the channel's parent; {previous} is the channel departed from. The default
    // channel-title format uses {parent}, so it must actually be substituted.
    FormatValues v;
    v.channel = "Racing";
    v.parent = "Games";
    v.previous = "Lobby";
    CHECK_EQ(format_template("{parent} / {channel}", v), std::string("Games / Racing"));
    CHECK_EQ(format_template("{previous} -> {channel}", v), std::string("Lobby -> Racing"));
}

TEST(layout, every_documented_placeholder_is_substituted) {
    FormatValues v;
    v.name = "N"; v.channel = "C"; v.parent = "P"; v.previous = "V";
    v.count = "1"; v.status = "S"; v.message = "M"; v.server = "R"; v.time = "T";
    const std::string out = format_template(
        "{name}{channel}{parent}{previous}{count}{status}{message}{server}{time}", v);
    CHECK_EQ(out, std::string("NCPV1SMRT"));
    CHECK(out.find('{') == std::string::npos);
}

TEST(layout, an_unknown_placeholder_is_left_visible) {
    FormatValues v;
    v.name = "Alice";
    CHECK_EQ(format_template("{name} {nmae}", v), std::string("Alice {nmae}"));
    CHECK_EQ(format_template("unclosed {name", v), std::string("unclosed {name"));
    CHECK_EQ(format_template("", v), std::string(""));
}

TEST(layout, the_layout_is_computable_at_every_tested_resolution) {
    Config cfg = Config::defaults();
    std::vector<UserState> users;
    for (int i = 0; i < 12; ++i) {
        users.push_back(make_user(("u" + std::to_string(i) + "=").c_str(),
                                  "AReasonablyLongNickname"));
    }
    const OverlayState s = connected_state(users);
    for (const Viewport vp : {Viewport{800, 600}, Viewport{1280, 720}, Viewport{1366, 768},
                              Viewport{1920, 1080}, Viewport{2560, 1440}, Viewport{3440, 1440},
                              Viewport{3840, 2160}, Viewport{5120, 1440}}) {
        const LayoutResult r = compute_layout(s, cfg, vp, stub_measure(), 1.0f, nullptr);
        CHECK(r.users.visible);
        CHECK(r.users.rect.w > 0.0f);
        CHECK(r.users.rect.h > 0.0f);
        CHECK(r.title.rect.w > 0.0f);
        for (const auto& row : r.users.rows) {
            CHECK(row.name.width <= cfg.user_list.max_name_width + 0.5f);
        }
    }
}

TEST(layout, the_global_scale_multiplies_sizes) {
    Config cfg = Config::defaults();
    const OverlayState s = connected_state({make_user("a=", "Alice")});
    const LayoutResult small =
        compute_layout(s, cfg, Viewport{1920, 1080}, stub_measure(), 0.0f, nullptr);
    cfg.general.scale = 2.0f;
    const LayoutResult large =
        compute_layout(s, cfg, Viewport{1920, 1080}, stub_measure(), 0.0f, nullptr);
    CHECK(large.users.rows[0].rect.h > small.users.rows[0].rect.h);
    CHECK(large.title.font_size > small.title.font_size);
}
