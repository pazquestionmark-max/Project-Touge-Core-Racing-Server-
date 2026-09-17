// SPDX-License-Identifier: MIT
// The in-game settings window.
//
// Registered with reshade::register_overlay, so ReShade calls it only while its menu is open —
// which is exactly when the user is configuring and when ReShade is already blocking game input.
// The HUD itself is drawn by a separate every-frame callback and never takes input.
#ifndef TSRO_SETTINGS_UI_HPP
#define TSRO_SETTINGS_UI_HPP

#include <string>
#include <vector>

#include "renderer.hpp"
#include "tsro/config.hpp"
#include "tsro/overlay_client.hpp"
#include "tsro/profile_store.hpp"

namespace tsro::overlay {

/// What the settings window is asking the host add-on to do. Returned rather than performed
/// in-place so file and IPC work happens in the add-on's own code path, not mid-ImGui-frame.
struct SettingsActions {
    bool config_changed = false;      ///< re-layout and re-send the subscription
    bool save_requested = false;
    bool reload_requested = false;
    bool reconnect_requested = false;
    bool subscription_changed = false;
    std::string switch_to_profile;    ///< non-empty: load this profile
};

class SettingsUi {
public:
    /// Draws the window. `config` is edited in place; the caller persists it when asked.
    SettingsActions draw(Config& config, const LinkDiagnostics& diagnostics,
                         const OverlayFrame& frame, ProfileStore& profiles,
                         const FrameStats& stats, const ConfigDiagnostics& config_diagnostics);

    /// True while the Appearance/Layout tabs want the HUD drawn from mock data instead of the
    /// live model, so the user can see every indicator without waiting for someone to speak.
    bool preview_active() const noexcept { return preview_active_; }

    void refresh_profiles(ProfileStore& profiles);
    void set_status(std::string message, bool error);

private:
    void tab_general(Config& config, SettingsActions& actions);
    void tab_appearance(Config& config, SettingsActions& actions);
    void tab_layout(Config& config, SettingsActions& actions);
    void tab_users(Config& config, const OverlayFrame& frame, SettingsActions& actions);
    void tab_channels(Config& config, const OverlayFrame& frame, SettingsActions& actions);
    void tab_indicators(Config& config, SettingsActions& actions);
    void tab_notifications(Config& config, SettingsActions& actions);
    void tab_chat(Config& config, SettingsActions& actions);
    void tab_animation(Config& config, SettingsActions& actions);
    void tab_integration(Config& config, SettingsActions& actions);
    void tab_profiles(Config& config, ProfileStore& profiles, SettingsActions& actions);
    void tab_diagnostics(const LinkDiagnostics& diagnostics, const OverlayFrame& frame,
                         const FrameStats& stats, const ConfigDiagnostics& config_diagnostics,
                         Config& config, SettingsActions& actions);

    bool preview_active_ = false;
    std::vector<ProfileInfo> profiles_;
    std::string status_;
    bool status_is_error_ = false;
    char new_profile_name_[64] = "";
    char export_path_[512] = "";
    char import_path_[512] = "";
    char user_filter_[64] = "";
    char executable_name_[128] = "";
    int selected_profile_ = 0;
};

}  // namespace tsro::overlay

#endif  // TSRO_SETTINGS_UI_HPP
