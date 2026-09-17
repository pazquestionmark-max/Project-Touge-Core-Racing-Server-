// SPDX-License-Identifier: MIT
// TeamSpeak 3 client plugin entry points.
//
// This file is deliberately thin: it does nothing but translate TeamSpeak's C callbacks into
// PluginCore calls. All the behaviour lives in PluginCore and TsState, which are testable
// without TeamSpeak. Every export here is `extern "C"` because TeamSpeak resolves them by name.
//
// Threading: TeamSpeak invokes these on its own thread. None of them blocks on I/O — the IPC
// server owns every pipe handle on its own threads (docs/architecture.md §6.3).
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "plugin_core.hpp"
#include "ts_query_ts3.hpp"
#include "tsro/log.hpp"

#include "plugin_definitions.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_rare_definitions.h"
#include "ts3_functions.h"

#ifdef _WIN32
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace {

/// Plugin API version this build targets. TeamSpeak refuses to load a plugin whose value does
/// not match the version the client implements, which is why it is a hard constant and not a
/// guess: see docs/compatibility.md for the client versions that pair with it.
constexpr int kPluginApiVersion = 26;
constexpr const char* kPluginName = "TeamSpeak ReShade Overlay";
constexpr const char* kPluginVersion = "1.0.0";
constexpr const char* kPluginAuthor = "TeamSpeak ReShade Overlay contributors";
constexpr const char* kPluginDescription =
    "Publishes your current channel, its members and their voice states to the ReShade overlay "
    "add-on over a local named pipe. Sends nothing to the network. Chat is forwarded only for "
    "the categories the overlay explicitly subscribes to, and private messages are off unless "
    "you turn them on.";

TS3Functions g_ts;
std::unique_ptr<tsro::plugin::Ts3Query> g_query;
std::unique_ptr<tsro::plugin::PluginCore> g_core;
std::string g_plugin_id;
std::string g_info_buffer;

/// TeamSpeak takes ownership of strings returned from some callbacks and frees them with free(),
/// so they must be allocated with malloc rather than new.
char* duplicate_for_ts(const std::string& text) {
    const std::size_t size = text.size() + 1;
    char* buffer = static_cast<char*>(std::malloc(size));
    if (buffer == nullptr) return nullptr;
    std::memcpy(buffer, text.c_str(), size);
    return buffer;
}

tsro::plugin::TsConnectStatus map_connect_status(int status) {
    switch (status) {
        case STATUS_DISCONNECTED: return tsro::plugin::TsConnectStatus::Disconnected;
        case STATUS_CONNECTING: return tsro::plugin::TsConnectStatus::Connecting;
        case STATUS_CONNECTED: return tsro::plugin::TsConnectStatus::Connected;
        case STATUS_CONNECTION_ESTABLISHING:
            return tsro::plugin::TsConnectStatus::Establishing;
        case STATUS_CONNECTION_ESTABLISHED: return tsro::plugin::TsConnectStatus::Established;
        default: return tsro::plugin::TsConnectStatus::Disconnected;
    }
}

tsro::plugin::TsTalkStatus map_talk_status(int status) {
    switch (status) {
        case STATUS_TALKING: return tsro::plugin::TsTalkStatus::Talking;
        case STATUS_TALKING_WHILE_DISABLED:
            return tsro::plugin::TsTalkStatus::TalkingWhileDisabled;
        default: return tsro::plugin::TsTalkStatus::NotTalking;
    }
}

bool map_text_target(anyID target_mode, tsro::plugin::TsTextTarget& out) {
    switch (target_mode) {
        case TextMessageTarget_CLIENT: out = tsro::plugin::TsTextTarget::Client; return true;
        case TextMessageTarget_CHANNEL: out = tsro::plugin::TsTextTarget::Channel; return true;
        case TextMessageTarget_SERVER: out = tsro::plugin::TsTextTarget::Server; return true;
        default: return false;  // a target we do not model is dropped, never guessed at
    }
}

std::string configuration_directory() {
    char path[1024];
    path[0] = '\0';
    if (g_ts.getConfigPath != nullptr) g_ts.getConfigPath(path, sizeof(path));
    path[sizeof(path) - 1] = '\0';
    return std::string(path);
}

std::string safe(const char* text) { return text != nullptr ? std::string(text) : std::string(); }

}  // namespace

extern "C" {

PLUGIN_EXPORT const char* ts3plugin_name() { return kPluginName; }
PLUGIN_EXPORT const char* ts3plugin_version() { return kPluginVersion; }
PLUGIN_EXPORT int ts3plugin_apiVersion() { return kPluginApiVersion; }
PLUGIN_EXPORT const char* ts3plugin_author() { return kPluginAuthor; }
PLUGIN_EXPORT const char* ts3plugin_description() { return kPluginDescription; }

PLUGIN_EXPORT void ts3plugin_setFunctionPointers(const struct TS3Functions functions) {
    g_ts = functions;
}

PLUGIN_EXPORT int ts3plugin_init() {
    std::string log_path = configuration_directory();
    if (!log_path.empty() && log_path.back() != '/' && log_path.back() != '\\') {
        log_path.push_back('/');
    }
    log_path += "tsro-plugin.log";
    tsro::Logger::instance().configure(tsro::LogLevel::Info, true, log_path, 512);
    TSRO_INFO("plugin", std::string("initialising ") + kPluginName + " " + kPluginVersion);

    g_query = std::make_unique<tsro::plugin::Ts3Query>(g_ts);

    tsro::plugin::PluginCoreOptions options;
    options.plugin_version = kPluginVersion;
    options.plugin_api_version = kPluginApiVersion;
    g_core = std::make_unique<tsro::plugin::PluginCore>(*g_query, std::move(options));

    // Where the client keeps settings.db, and so where its contact list lives.
    g_core->set_config_directory(configuration_directory());

    std::string error;
    if (!g_core->start(error)) {
        TSRO_ERROR("plugin", "could not start the overlay endpoint: " + error);
        if (g_ts.printMessageToCurrentTab != nullptr) {
            const std::string message =
                "[TeamSpeak ReShade Overlay] could not open the local pipe: " + error +
                ". The overlay will not receive data. See tsro-plugin.log.";
            g_ts.printMessageToCurrentTab(message.c_str());
        }
        // Returning 1 would unload the plugin and lose the diagnostics with it. Staying loaded
        // in a degraded state lets the user read the reason in the plugin's info panel.
        g_core.reset();
        g_query.reset();
        return 0;
    }

    // Adopt whatever connection is already active: the plugin is commonly enabled while the
    // user is already sitting in a channel, and they should not have to reconnect.
    if (g_ts.getCurrentServerConnectionHandlerID != nullptr) {
        const uint64 current = g_ts.getCurrentServerConnectionHandlerID();
        if (current != 0) {
            int status = STATUS_DISCONNECTED;
            if (g_ts.getConnectionStatus != nullptr &&
                g_ts.getConnectionStatus(current, &status) == ERROR_ok &&
                status >= STATUS_CONNECTED) {
                g_core->on_connect_status_changed(current, map_connect_status(status), 0);
            }
        }
    }
    return 0;
}

PLUGIN_EXPORT void ts3plugin_shutdown() {
    TSRO_INFO("plugin", "shutting down");
    // Order matters: the core stops the IPC server, which joins its threads, before the query
    // adapter holding TeamSpeak's function table goes away.
    if (g_core) g_core->stop();
    g_core.reset();
    g_query.reset();
    if (!g_plugin_id.empty()) g_plugin_id.clear();
}

PLUGIN_EXPORT void ts3plugin_registerPluginID(const char* id) { g_plugin_id = safe(id); }

PLUGIN_EXPORT int ts3plugin_requestAutoload() { return 0; }

PLUGIN_EXPORT const char* ts3plugin_infoTitle() { return "ReShade Overlay"; }

PLUGIN_EXPORT void ts3plugin_infoData(uint64 serverConnectionHandlerID, uint64 id,
                                      enum PluginItemType type, char** data) {
    (void)serverConnectionHandlerID;
    (void)id;
    if (data == nullptr) return;
    *data = nullptr;
    if (type != PLUGIN_SERVER) return;  // the server row is where a status summary belongs

    g_info_buffer.clear();
    if (!g_core) {
        g_info_buffer = "not running - see tsro-plugin.log";
    } else {
        for (const std::string& line : g_core->diagnostics()) {
            if (!g_info_buffer.empty()) g_info_buffer += "\n";
            g_info_buffer += line;
        }
    }
    *data = duplicate_for_ts(g_info_buffer);
}

PLUGIN_EXPORT void ts3plugin_freeMemory(void* data) { std::free(data); }

PLUGIN_EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID,
                                                        int newStatus, unsigned int errorNumber) {
    if (!g_core) return;
    g_core->on_connect_status_changed(serverConnectionHandlerID, map_connect_status(newStatus),
                                      errorNumber);
}

PLUGIN_EXPORT void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                               uint64 oldChannelID, uint64 newChannelID,
                                               int visibility, const char* moveMessage) {
    (void)visibility;
    (void)moveMessage;
    if (!g_core) return;
    // oldChannelID == 0 means the client just connected to the server; newChannelID == 0 means
    // it disconnected. Both arrive through this same callback.
    const tsro::plugin::MoveCause cause = oldChannelID == 0
                                              ? tsro::plugin::MoveCause::Connected
                                              : (newChannelID == 0
                                                     ? tsro::plugin::MoveCause::Disconnected
                                                     : tsro::plugin::MoveCause::Moved);
    g_core->on_client_moved(serverConnectionHandlerID, static_cast<std::uint16_t>(clientID),
                            oldChannelID, newChannelID, cause);
}

PLUGIN_EXPORT void ts3plugin_onClientMoveSubscriptionEvent(uint64 serverConnectionHandlerID,
                                                           anyID clientID, uint64 oldChannelID,
                                                           uint64 newChannelID, int visibility) {
    (void)oldChannelID;
    (void)visibility;
    if (!g_core) return;
    // A visibility change, not a real move: the client was already there, we just started
    // seeing it. Treated as a join into our channel so the roster stays complete.
    g_core->on_client_moved(serverConnectionHandlerID, static_cast<std::uint16_t>(clientID),
                            0, newChannelID, tsro::plugin::MoveCause::Connected);
}

PLUGIN_EXPORT void ts3plugin_onClientMoveTimeoutEvent(uint64 serverConnectionHandlerID,
                                                      anyID clientID, uint64 oldChannelID,
                                                      uint64 newChannelID, int visibility,
                                                      const char* timeoutMessage) {
    (void)visibility;
    (void)timeoutMessage;
    if (!g_core) return;
    g_core->on_client_moved(serverConnectionHandlerID, static_cast<std::uint16_t>(clientID),
                            oldChannelID, newChannelID, tsro::plugin::MoveCause::Timeout);
}

PLUGIN_EXPORT void ts3plugin_onClientMoveMovedEvent(uint64 serverConnectionHandlerID,
                                                    anyID clientID, uint64 oldChannelID,
                                                    uint64 newChannelID, int visibility,
                                                    anyID moverID, const char* moverName,
                                                    const char* moverUniqueIdentifier,
                                                    const char* moveMessage) {
    (void)visibility;
    (void)moverID;
    (void)moverName;
    (void)moverUniqueIdentifier;
    (void)moveMessage;
    if (!g_core) return;
    g_core->on_client_moved(serverConnectionHandlerID, static_cast<std::uint16_t>(clientID),
                            oldChannelID, newChannelID, tsro::plugin::MoveCause::Moved);
}

PLUGIN_EXPORT void ts3plugin_onClientKickFromChannelEvent(
    uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID,
    int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier,
    const char* kickMessage) {
    (void)visibility;
    (void)kickerID;
    (void)kickerName;
    (void)kickerUniqueIdentifier;
    (void)kickMessage;
    if (!g_core) return;
    g_core->on_client_moved(serverConnectionHandlerID, static_cast<std::uint16_t>(clientID),
                            oldChannelID, newChannelID, tsro::plugin::MoveCause::Kicked);
}

PLUGIN_EXPORT void ts3plugin_onClientKickFromServerEvent(
    uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID,
    int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier,
    const char* kickMessage) {
    (void)visibility;
    (void)kickerID;
    (void)kickerName;
    (void)kickerUniqueIdentifier;
    (void)kickMessage;
    if (!g_core) return;
    g_core->on_client_moved(serverConnectionHandlerID, static_cast<std::uint16_t>(clientID),
                            oldChannelID, newChannelID, tsro::plugin::MoveCause::Kicked);
}

PLUGIN_EXPORT void ts3plugin_onClientBanFromServerEvent(
    uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID,
    int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier,
    uint64 time, const char* kickMessage) {
    (void)visibility;
    (void)kickerID;
    (void)kickerName;
    (void)kickerUniqueIdentifier;
    (void)time;
    (void)kickMessage;
    if (!g_core) return;
    g_core->on_client_moved(serverConnectionHandlerID, static_cast<std::uint16_t>(clientID),
                            oldChannelID, newChannelID, tsro::plugin::MoveCause::Banned);
}

PLUGIN_EXPORT void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status,
                                                     int isReceivedWhisper, anyID clientID) {
    if (!g_core) return;
    // isReceivedWhisper is the only whisper signal the plugin API provides, and it covers the
    // incoming direction only (docs/protocol.md §6.1).
    g_core->on_talk_status_changed(serverConnectionHandlerID, map_talk_status(status),
                                   isReceivedWhisper != 0,
                                   static_cast<std::uint16_t>(clientID));
}

PLUGIN_EXPORT void ts3plugin_onUpdateClientEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                                 anyID invokerID, const char* invokerName,
                                                 const char* invokerUniqueIdentifier) {
    (void)invokerID;
    (void)invokerName;
    (void)invokerUniqueIdentifier;
    if (!g_core) return;
    g_core->on_client_updated(serverConnectionHandlerID, static_cast<std::uint16_t>(clientID));
}

PLUGIN_EXPORT void ts3plugin_onClientSelfVariableUpdateEvent(uint64 serverConnectionHandlerID,
                                                             int flag, const char* oldValue,
                                                             const char* newValue) {
    (void)flag;
    (void)oldValue;
    (void)newValue;
    if (!g_core) return;
    g_core->on_self_variable_updated(serverConnectionHandlerID);
}

PLUGIN_EXPORT void ts3plugin_onClientDisplayNameChanged(uint64 serverConnectionHandlerID,
                                                        anyID clientID, const char* displayName,
                                                        const char* uniqueClientIdentifier) {
    (void)displayName;
    (void)uniqueClientIdentifier;
    if (!g_core) return;
    g_core->on_client_updated(serverConnectionHandlerID, static_cast<std::uint16_t>(clientID));
}

PLUGIN_EXPORT void ts3plugin_onUpdateChannelEvent(uint64 serverConnectionHandlerID,
                                                  uint64 channelID) {
    if (!g_core) return;
    g_core->on_channel_updated(serverConnectionHandlerID, channelID);
}

PLUGIN_EXPORT void ts3plugin_onUpdateChannelEditedEvent(uint64 serverConnectionHandlerID,
                                                        uint64 channelID, anyID invokerID,
                                                        const char* invokerName,
                                                        const char* invokerUniqueIdentifier) {
    (void)invokerID;
    (void)invokerName;
    (void)invokerUniqueIdentifier;
    if (!g_core) return;
    g_core->on_channel_updated(serverConnectionHandlerID, channelID);
}

PLUGIN_EXPORT void ts3plugin_onChannelSubscribeFinishedEvent(uint64 serverConnectionHandlerID) {
    if (!g_core) return;
    g_core->on_subscription_finished(serverConnectionHandlerID);
}

PLUGIN_EXPORT int ts3plugin_onTextMessageEvent(uint64 serverConnectionHandlerID, anyID targetMode,
                                               anyID toID, anyID fromID, const char* fromName,
                                               const char* fromUniqueIdentifier,
                                               const char* message, int ffIgnored) {
    (void)toID;
    if (ffIgnored != 0) return 0;  // the client is ignoring this sender; respect that
    tsro::plugin::TsTextTarget target;
    if (g_core != nullptr && map_text_target(targetMode, target)) {
        g_core->on_text_message(serverConnectionHandlerID, target,
                                static_cast<std::uint16_t>(fromID), safe(fromName),
                                safe(fromUniqueIdentifier), safe(message));
    }
    return 0;  // never consume the message: TeamSpeak must still display it normally
}

PLUGIN_EXPORT void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) {
    if (!g_core) return;
    g_core->on_current_server_changed(serverConnectionHandlerID);
}

PLUGIN_EXPORT void ts3plugin_onServerStopEvent(uint64 serverConnectionHandlerID,
                                               const char* shutdownMessage) {
    (void)shutdownMessage;
    if (!g_core) return;
    g_core->on_server_stopped(serverConnectionHandlerID);
}

}  // extern "C"
