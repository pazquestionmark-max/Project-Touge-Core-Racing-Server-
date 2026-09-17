// SPDX-License-Identifier: MIT
// ReShade add-on entry point (Component A).
//
// Two registrations, each chosen for when ReShade invokes it (verified in ReShade's source, see
// docs/architecture.md §1.2):
//
//   * addon_event::reshade_overlay  -- called every frame, between ImGui::NewFrame and
//     ImGui::EndFrame. ReShade's early-out explicitly keeps building an ImGui frame when an
//     add-on has subscribed to this event, which is what makes an always-on HUD possible.
//     We draw into the background draw list, so we add no draw call batch of our own and take
//     no input.
//
//   * register_overlay("...") -- called only while ReShade's menu is open. That is exactly when
//     the user is configuring and when ReShade is already blocking game input, so it is the
//     right home for the settings window.
//
// Nothing here blocks: the IPC connection lives on OverlayClient's own thread and the render
// callback only reads a triple-buffered frame.
#include <atomic>
#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>
// Order matters: reshade.hpp defines the ImGui:: and ImDrawList:: members that imgui.h only
// declares, routing them through ReShade's function table.
#include <reshade.hpp>

#include "renderer.hpp"
#include "settings_ui.hpp"
#include "tsro/log.hpp"
#include "tsro/overlay_client.hpp"
#include "tsro/profile_store.hpp"

extern "C" __declspec(dllexport) const char* NAME = "TeamSpeak Overlay";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Shows your current TeamSpeak channel, who is in it and their voice states, fed by the "
    "TeamSpeak ReShade Overlay plugin over a local named pipe. Opens no network connection.";

namespace {

constexpr char kComponent[] = "addon";

/// Everything the add-on owns, so lifetime is one object rather than a scatter of globals.
struct AddonState {
    tsro::Config config = tsro::Config::defaults();
    tsro::ConfigDiagnostics config_diagnostics;
    std::unique_ptr<tsro::ProfileStore> profiles;
    std::unique_ptr<tsro::OverlayClient> client;
    tsro::overlay::Renderer renderer;
    tsro::overlay::SettingsUi settings;

    /// Guards `config` only. The render callback reads it and the settings callback writes it;
    /// both run on the same thread inside ReShade's ImGui frame, so this is uncontended in
    /// practice and exists to make the ordering explicit rather than assumed.
    std::mutex config_mutex;

    tsro::OverlayState preview;
    std::vector<tsro::ChatMessage> preview_chat;
    std::atomic<bool> started{false};
    std::string profile_name = "default";
};

AddonState* g_state = nullptr;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

tsro::proto::ConfigurationUpdatedPayload subscription_from(const tsro::Config& config) {
    tsro::proto::ConfigurationUpdatedPayload payload;
    // Only ask for what the user actually chose to display. A category left off is filtered in
    // the plugin and never reaches this process at all.
    payload.chat.channel = config.chat.placement.visible && config.chat.show_channel_messages;
    payload.chat.server = config.chat.placement.visible && config.chat.show_server_messages;
    payload.chat.priv = config.chat.placement.visible && config.chat.show_private_messages;
    payload.max_chat_length = config.chat.max_message_length;
    payload.want_speaking_events = true;
    return payload;
}

void apply_logging(const tsro::Config& config, const std::string& root) {
    std::string path = config.logging.file_name;
    if (path.empty() && !root.empty()) path = root + "/tsro-overlay.log";
    tsro::Logger::instance().configure(tsro::parse_log_level(config.logging.level), 
                                       config.logging.to_file, path, config.logging.max_file_kb);
    tsro::Logger::instance().set_include_message_content(config.logging.include_message_content);
}

void start_client(AddonState& state) {
    tsro::OverlayClientConfig client_config;
    client_config.endpoint = state.config.integration.pipe_name;
    client_config.reconnect_initial_ms = state.config.integration.reconnect_initial_ms;
    client_config.reconnect_max_ms = state.config.integration.reconnect_max_ms;
    client_config.stale_after_ms = state.config.integration.stale_after_ms;
    client_config.ping_interval_ms = state.config.integration.ping_interval_ms;
    client_config.chat_history = static_cast<std::size_t>(state.config.chat.history_size);
    client_config.subscription = subscription_from(state.config);
    client_config.hello.client = "reshade-addon";
    client_config.hello.client_version = TSRO_VERSION;
    client_config.hello.process = tsro::current_executable_name();
    client_config.hello.pid = 0;

    state.client = std::make_unique<tsro::OverlayClient>(std::move(client_config));
    if (state.config.integration.auto_connect) state.client->start();
}

void load_configuration(AddonState& state) {
    const std::string root = tsro::ProfileStore::default_root();
    state.profiles = std::make_unique<tsro::ProfileStore>(root);
    std::string error;
    if (!state.profiles->ensure_root(error)) {
        TSRO_WARN(kComponent, "configuration directory unavailable: " + error);
    }

    // Per-game profile selection: exact executable match, then the default. One lookup at load,
    // not per frame.
    std::string profile = "default";
    tsro::ConfigDiagnostics probe;
    const tsro::Config probe_config = state.profiles->load("default", probe);
    if (probe_config.general.auto_profile_by_executable) {
        const std::string executable = tsro::current_executable_name();
        if (!executable.empty()) profile = state.profiles->profile_for_executable(executable);
    }

    state.config_diagnostics = tsro::ConfigDiagnostics{};
    state.config = state.profiles->load(profile, state.config_diagnostics);
    state.profile_name = profile;
    apply_logging(state.config, state.profiles->root());

    TSRO_INFO(kComponent, "loaded profile '" + profile + "'");
    for (const tsro::ConfigIssue& issue : state.config_diagnostics.issues) {
        if (issue.severity == tsro::ConfigIssue::Severity::Info) continue;
        TSRO_WARN(kComponent, "configuration: " + issue.path + " " + issue.message);
    }
}

/// Called every frame by ReShade, inside its ImGui frame.
void on_reshade_overlay(reshade::api::effect_runtime* runtime) {
    AddonState* state = g_state;
    if (state == nullptr || !state->started.load(std::memory_order_acquire)) return;
    if (runtime == nullptr) return;

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    if (draw_list == nullptr) return;

    // The viewport comes from ReShade's own API rather than ImGuiIO::DisplaySize.
    //
    // Reading a field off ImGuiIO means trusting that this add-on's imgui.h lays the struct out
    // exactly as ReShade's ImGui build does. That assumption is what crashed the game when the
    // font list walked ImFontAtlas, and it is the same assumption here. get_screenshot_width_and_height
    // is a virtual call across a versioned interface, so there is no layout to guess at.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    tsro::Viewport viewport;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    if (viewport.width < 1.0f || viewport.height < 1.0f) return;

    const std::int64_t now = now_ms();
    const tsro::OverlayFrame& frame = state->client->latest();

    std::lock_guard<std::mutex> lock(state->config_mutex);
    const std::vector<tsro::OverlayEvent> events = state->client->drain_events();
    state->renderer.submit_events(events, state->config, now);

    const bool preview = state->settings.preview_active();
    if (preview && state->preview.users.empty()) {
        state->preview = tsro::overlay::preview_state();
        state->preview_chat = tsro::overlay::preview_chat();
    }

    state->renderer.draw(draw_list, state->config, frame, viewport, now,
                         preview ? &state->preview : nullptr,
                         preview ? &state->preview_chat : nullptr);
}

/// Called by ReShade only while its menu is open.
void on_settings_overlay(reshade::api::effect_runtime* runtime) {
    (void)runtime;
    AddonState* state = g_state;
    if (state == nullptr || !state->started.load(std::memory_order_acquire)) return;

    const tsro::OverlayFrame& frame = state->client->latest();
    const tsro::LinkDiagnostics diagnostics = state->client->diagnostics();

    std::lock_guard<std::mutex> lock(state->config_mutex);
    const tsro::overlay::SettingsActions actions =
        state->settings.draw(state->config, diagnostics, frame, *state->profiles,
                             state->renderer.stats(), state->config_diagnostics);

    if (actions.config_changed) {
        state->config.clamp(state->config_diagnostics);
        state->renderer.invalidate();
        apply_logging(state->config, state->profiles->root());
        state->client->set_stale_after_ms(state->config.integration.stale_after_ms);
    }
    if (actions.subscription_changed) {
        state->client->update_subscription(subscription_from(state->config));
    }
    if (actions.reconnect_requested) state->client->request_reconnect();

    if (actions.save_requested) {
        std::string error;
        if (state->profiles->save(state->profile_name, state->config, error)) {
            state->settings.set_status("Saved.", false);
            state->settings.refresh_profiles(*state->profiles);
        } else {
            state->settings.set_status(error, true);
        }
    }
    if (actions.reload_requested) {
        state->config_diagnostics = tsro::ConfigDiagnostics{};
        state->config = state->profiles->load(state->profile_name, state->config_diagnostics);
        state->renderer.invalidate();
        state->client->update_subscription(subscription_from(state->config));
        state->settings.set_status("Reloaded from disk.", false);
    }
    if (!actions.switch_to_profile.empty()) {
        state->config_diagnostics = tsro::ConfigDiagnostics{};
        state->config =
            state->profiles->load(actions.switch_to_profile, state->config_diagnostics);
        state->profile_name = actions.switch_to_profile;
        state->renderer.invalidate();
        state->renderer.clear_notifications();
        state->client->update_subscription(subscription_from(state->config));
        apply_logging(state->config, state->profiles->root());
        state->settings.set_status("Loaded profile '" + actions.switch_to_profile + "'.", false);
    }
}

bool initialise(HMODULE module) {
    // register_addon also resolves ReShade's ImGui function table, and fails if this add-on was
    // built against an ImGui version ReShade does not export. Failing here rather than drawing
    // through a null table is what turns a version mismatch into "the add-on did not load"
    // instead of a crash inside the game.
    if (!reshade::register_addon(module)) return false;

    g_state = new AddonState();
    load_configuration(*g_state);
    start_client(*g_state);
    g_state->settings.refresh_profiles(*g_state->profiles);
    g_state->started.store(true, std::memory_order_release);

    reshade::register_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay);
    reshade::register_overlay("TeamSpeak Overlay", &on_settings_overlay);
    TSRO_INFO(kComponent, "add-on initialised");
    return true;
}

void shutdown(HMODULE module) {
    if (g_state != nullptr) {
        g_state->started.store(false, std::memory_order_release);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay);
        reshade::unregister_overlay("TeamSpeak Overlay", &on_settings_overlay);
        // Stop the IPC thread before the state it references is destroyed.
        if (g_state->client) g_state->client->stop();
        delete g_state;
        g_state = nullptr;
    }
    reshade::unregister_addon(module);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // No thread is created here: OverlayClient starts its own, which DllMain is allowed
            // to request because CreateThread does not take the loader lock's critical path for
            // work we do not wait on.
            DisableThreadLibraryCalls(module);
            if (!initialise(module)) return FALSE;
            break;
        case DLL_PROCESS_DETACH:
            shutdown(module);
            break;
        default:
            break;
    }
    return TRUE;
}
