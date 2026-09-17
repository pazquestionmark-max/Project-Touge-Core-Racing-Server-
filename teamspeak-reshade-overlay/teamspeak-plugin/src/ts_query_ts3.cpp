// SPDX-License-Identifier: MIT
// Adapter from this project's property enums onto the real TeamSpeak 3 Plugin SDK enumerators.
//
// This is the only file that includes the TeamSpeak headers or knows their constants. Keeping
// the mapping in one reviewable place is what lets the rest of the plugin be built and tested
// without the SDK, and means no SDK value is ever hardcoded as a bare integer.
#include "ts_query_ts3.hpp"

#include <cstring>
#include <vector>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_rare_definitions.h"
#include "ts3_functions.h"

namespace tsro::plugin {
namespace {

/// Frees a string TeamSpeak allocated for us and copies it out. Every getClient*AsString call
/// transfers ownership, so forgetting this leaks once per property per event.
std::string take(const TS3Functions& ts, char* owned) {
    if (owned == nullptr) return std::string();
    std::string out(owned);
    ts.freeMemory(owned);
    return out;
}

std::size_t map(UserFlag flag) {
    switch (flag) {
        case UserFlag::InputMuted: return CLIENT_INPUT_MUTED;
        // CLIENT_OUTPUT_MUTED implies microphone mute; CLIENT_OUTPUTONLY_MUTED is the speaker
        // state on its own, which is what the overlay must show as "speakers muted".
        case UserFlag::OutputMuted: return CLIENT_OUTPUTONLY_MUTED;
        case UserFlag::InputHardware: return CLIENT_INPUT_HARDWARE;
        case UserFlag::OutputHardware: return CLIENT_OUTPUT_HARDWARE;
        case UserFlag::InputDeactivated: return CLIENT_INPUT_DEACTIVATED;
        case UserFlag::Away: return CLIENT_AWAY;
        case UserFlag::Recording: return CLIENT_IS_RECORDING;
        case UserFlag::ChannelCommander: return CLIENT_IS_CHANNEL_COMMANDER;
        case UserFlag::PrioritySpeaker: return CLIENT_IS_PRIORITY_SPEAKER;
        case UserFlag::IsTalker: return CLIENT_IS_TALKER;
        case UserFlag::HasAvatar: return CLIENT_FLAG_AVATAR;
        case UserFlag::LocallyMuted: return CLIENT_IS_MUTED;
        case UserFlag::TalkPower: return CLIENT_TALK_POWER;
    }
    return CLIENT_ENDMARKER;
}

std::size_t map(UserText field) {
    switch (field) {
        case UserText::UniqueIdentifier: return CLIENT_UNIQUE_IDENTIFIER;
        case UserText::Nickname: return CLIENT_NICKNAME;
        case UserText::AwayMessage: return CLIENT_AWAY_MESSAGE;
        case UserText::Country: return CLIENT_COUNTRY;
    }
    return CLIENT_ENDMARKER;
}

std::size_t map(ChannelText field) {
    switch (field) {
        case ChannelText::Name: return CHANNEL_NAME;
        case ChannelText::Topic: return CHANNEL_TOPIC;
    }
    return CHANNEL_ENDMARKER;
}

std::size_t map(ServerText field) {
    switch (field) {
        case ServerText::Name: return VIRTUALSERVER_NAME;
        case ServerText::UniqueIdentifier: return VIRTUALSERVER_UNIQUE_IDENTIFIER;
        case ServerText::ClientVersion: return VIRTUALSERVER_VERSION;
    }
    return VIRTUALSERVER_ENDMARKER;
}

}  // namespace

bool Ts3Query::user_flag(std::uint64_t server, std::uint16_t client, UserFlag flag, int& out) {
    const std::size_t property = map(flag);
    if (property == CLIENT_ENDMARKER) return false;
    int value = 0;
    if (ts_.getClientVariableAsInt(server, static_cast<anyID>(client), property, &value) !=
        ERROR_ok) {
        return false;  // unavailable for this client: the caller must keep the field absent
    }
    out = value;
    return true;
}

bool Ts3Query::user_text(std::uint64_t server, std::uint16_t client, UserText field,
                         std::string& out) {
    const std::size_t property = map(field);
    if (property == CLIENT_ENDMARKER) return false;
    char* value = nullptr;
    if (ts_.getClientVariableAsString(server, static_cast<anyID>(client), property, &value) !=
        ERROR_ok) {
        return false;
    }
    out = take(ts_, value);
    return true;
}

bool Ts3Query::self_flag(std::uint64_t server, UserFlag flag, int& out) {
    const std::size_t property = map(flag);
    if (property == CLIENT_ENDMARKER) return false;
    int value = 0;
    if (ts_.getClientSelfVariableAsInt(server, property, &value) != ERROR_ok) return false;
    out = value;
    return true;
}

bool Ts3Query::channel_text(std::uint64_t server, std::uint64_t channel, ChannelText field,
                            std::string& out) {
    const std::size_t property = map(field);
    if (property == CHANNEL_ENDMARKER) return false;
    char* value = nullptr;
    if (ts_.getChannelVariableAsString(server, channel, property, &value) != ERROR_ok) {
        return false;
    }
    out = take(ts_, value);
    return true;
}

bool Ts3Query::server_text(std::uint64_t server, ServerText field, std::string& out) {
    const std::size_t property = map(field);
    if (property == VIRTUALSERVER_ENDMARKER) return false;
    char* value = nullptr;
    if (ts_.getServerVariableAsString(server, property, &value) != ERROR_ok) return false;
    out = take(ts_, value);
    return true;
}

bool Ts3Query::own_client_id(std::uint64_t server, std::uint16_t& out) {
    anyID id = 0;
    if (ts_.getClientID(server, &id) != ERROR_ok || id == 0) return false;
    out = static_cast<std::uint16_t>(id);
    return true;
}

bool Ts3Query::channel_of_client(std::uint64_t server, std::uint16_t client,
                                 std::uint64_t& out) {
    uint64 channel = 0;
    if (ts_.getChannelOfClient(server, static_cast<anyID>(client), &channel) != ERROR_ok) {
        return false;
    }
    out = channel;
    return true;
}

bool Ts3Query::parent_of_channel(std::uint64_t server, std::uint64_t channel,
                                 std::uint64_t& out) {
    uint64 parent = 0;
    if (ts_.getParentChannelOfChannel(server, channel, &parent) != ERROR_ok) return false;
    out = parent;
    return true;
}

bool Ts3Query::channel_clients(std::uint64_t server, std::uint64_t channel,
                               std::vector<std::uint16_t>& out) {
    out.clear();
    anyID* list = nullptr;
    if (ts_.getChannelClientList(server, channel, &list) != ERROR_ok || list == nullptr) {
        return false;
    }
    for (int i = 0; list[i] != 0; ++i) out.push_back(static_cast<std::uint16_t>(list[i]));
    ts_.freeMemory(list);
    return true;
}

bool Ts3Query::display_name(std::uint64_t server, std::uint16_t client, std::string& out) {
    // getClientDisplayName writes into a caller-supplied buffer rather than allocating.
    char buffer[512];
    buffer[0] = '\0';
    if (ts_.getClientDisplayName(server, static_cast<anyID>(client), buffer, sizeof(buffer)) !=
        ERROR_ok) {
        return false;
    }
    buffer[sizeof(buffer) - 1] = '\0';
    out.assign(buffer);
    return !out.empty();
}

}  // namespace tsro::plugin
