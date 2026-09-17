// SPDX-License-Identifier: MIT
#include "ts_state.hpp"

#include <algorithm>

#include "tsro/protocol.hpp"

namespace tsro::plugin {
namespace {

/// Reads an int property into an optional<bool>: absent when TeamSpeak cannot tell us.
void read_bool(TsQuery& q, std::uint64_t server, std::uint16_t client, UserFlag flag,
               std::optional<bool>& out) {
    int value = 0;
    if (q.user_flag(server, client, flag, value)) out = value != 0;
    else out.reset();
}

void read_int(TsQuery& q, std::uint64_t server, std::uint16_t client, UserFlag flag,
              std::optional<int>& out) {
    int value = 0;
    if (q.user_flag(server, client, flag, value)) out = value;
    else out.reset();
}

template <typename T>
void note(std::vector<std::string>& changed, const char* name, const T& before, const T& after) {
    if (!(before == after)) changed.emplace_back(name);
}

}  // namespace

std::vector<std::string> supported_capabilities() {
    // Deliberately conservative: each entry corresponds to something Plugin API 26 actually
    // reports. Outgoing whisper, whisper targets and avatar images are absent because the API
    // does not expose them (docs/protocol.md §6).
    return {
        "chat", "whisper_incoming", "commander", "priority_speaker", "recording",
        "away",  "talk_power",       "locally_muted", "speaker_mute_independent",
        "hardware_state", "country",
    };
}

const UserState* TsState::find(std::uint16_t client) const {
    for (const auto& u : state_.users) {
        if (u.client_id == client) return &u;
    }
    return nullptr;
}

UserState* TsState::find_mutable(std::uint16_t client) {
    for (auto& u : state_.users) {
        if (u.client_id == client) return &u;
    }
    return nullptr;
}

bool TsState::read_user(std::uint64_t server, std::uint16_t client, UserState& out) const {
    TsQuery& q = query_;
    UserState u;
    u.client_id = client;

    // Without a unique identifier there is no stable identity, so the entry is unusable: it
    // could not be matched to a per-user override or correlated across a reconnect.
    if (!q.user_text(server, client, UserText::UniqueIdentifier, u.unique_id) ||
        u.unique_id.empty()) {
        return false;
    }
    q.user_text(server, client, UserText::Nickname, u.nickname);
    if (!q.display_name(server, client, u.display_name) || u.display_name.empty()) {
        u.display_name = u.nickname;
    }
    q.user_text(server, client, UserText::AwayMessage, u.away_message);
    q.user_text(server, client, UserText::Country, u.country);

    read_bool(q, server, client, UserFlag::InputMuted, u.input_muted);
    read_bool(q, server, client, UserFlag::OutputMuted, u.output_muted);
    read_bool(q, server, client, UserFlag::InputHardware, u.input_hardware);
    read_bool(q, server, client, UserFlag::OutputHardware, u.output_hardware);
    read_bool(q, server, client, UserFlag::Away, u.away);
    read_bool(q, server, client, UserFlag::Recording, u.recording);
    read_bool(q, server, client, UserFlag::ChannelCommander, u.channel_commander);
    read_bool(q, server, client, UserFlag::PrioritySpeaker, u.priority_speaker);
    read_bool(q, server, client, UserFlag::IsTalker, u.is_talker);
    read_bool(q, server, client, UserFlag::HasAvatar, u.has_avatar);
    read_int(q, server, client, UserFlag::TalkPower, u.talk_power);

    std::uint16_t own = 0;
    const bool have_own = q.own_client_id(server, own);
    u.is_self = have_own && own == client;

    if (u.is_self) {
        // CLIENT_INPUT_DEACTIVATED is documented as available only for our own client, and
        // CLIENT_IS_MUTED as available only for clients *other* than ourselves.
        int deactivated = 0;
        if (q.self_flag(server, UserFlag::InputDeactivated, deactivated)) {
            u.input_deactivated = deactivated != 0;
        }
        u.locally_muted.reset();
    } else {
        u.input_deactivated.reset();
        read_bool(q, server, client, UserFlag::LocallyMuted, u.locally_muted);
    }

    out = std::move(u);
    return true;
}

bool TsState::read_channel(std::uint64_t server, std::uint64_t channel, ChannelState& out) const {
    if (channel == 0) return false;
    TsQuery& q = query_;
    ChannelState c;
    c.id = channel;
    if (!q.channel_text(server, channel, ChannelText::Name, c.name)) return false;
    q.channel_text(server, channel, ChannelText::Topic, c.topic);

    if (q.parent_of_channel(server, channel, c.parent_id) && c.parent_id != 0) {
        q.channel_text(server, c.parent_id, ChannelText::Name, c.parent_name);
    }
    c.path = c.parent_name.empty() ? c.name : c.parent_name + "/" + c.name;
    out = std::move(c);
    return true;
}

void TsState::resynchronise(std::uint64_t server) {
    TsQuery& q = query_;
    state_.server.handler_id = server;
    q.server_text(server, ServerText::Name, state_.server.name);
    q.server_text(server, ServerText::UniqueIdentifier, state_.server.unique_id);

    std::uint16_t own = 0;
    if (!q.own_client_id(server, own)) {
        // Not yet in a usable state: keep the connection status but present no membership,
        // rather than showing a channel we cannot actually enumerate.
        state_.users.clear();
        state_.channel = ChannelState{};
        state_.synchronised = false;
        refresh_snapshot_cache();
        return;
    }

    q.user_text(server, own, UserText::UniqueIdentifier, state_.self_unique_id);

    std::uint64_t channel = 0;
    if (!q.channel_of_client(server, own, channel) || channel == 0) {
        state_.users.clear();
        state_.channel = ChannelState{};
        state_.synchronised = false;
        refresh_snapshot_cache();
        return;
    }
    read_channel(server, channel, state_.channel);

    // Preserve talking state across a resynchronisation: it is driven by a separate, higher
    // frequency callback and re-reading the roster must not blink every speaking indicator off.
    std::vector<std::pair<std::string, std::pair<bool, bool>>> talking;
    talking.reserve(state_.users.size());
    for (const auto& u : state_.users) {
        if (u.talking || u.whispering_to_me) {
            talking.emplace_back(u.unique_id, std::make_pair(u.talking, u.whispering_to_me));
        }
    }

    std::vector<std::uint16_t> clients;
    q.channel_clients(server, channel, clients);
    if (clients.size() > proto::kMaxUsersPerSnapshot) clients.resize(proto::kMaxUsersPerSnapshot);

    state_.users.clear();
    state_.users.reserve(clients.size());
    for (const std::uint16_t client : clients) {
        UserState u;
        if (!read_user(server, client, u)) continue;
        for (const auto& [uid, flags] : talking) {
            if (uid == u.unique_id) {
                u.talking = flags.first;
                u.whispering_to_me = flags.second;
                break;
            }
        }
        state_.users.push_back(std::move(u));
    }

    state_.synchronised = true;
    state_.last_event_ms = proto::now_unix_ms();
    refresh_snapshot_cache();
}

std::vector<std::string> TsState::refresh_user(std::uint64_t server, std::uint16_t client) {
    std::vector<std::string> changed;
    UserState* existing = find_mutable(client);
    if (existing == nullptr) return changed;

    UserState fresh;
    if (!read_user(server, client, fresh)) return changed;

    note(changed, "nickname", existing->nickname, fresh.nickname);
    note(changed, "display_name", existing->display_name, fresh.display_name);
    note(changed, "input_muted", existing->input_muted, fresh.input_muted);
    note(changed, "output_muted", existing->output_muted, fresh.output_muted);
    note(changed, "input_hardware", existing->input_hardware, fresh.input_hardware);
    note(changed, "output_hardware", existing->output_hardware, fresh.output_hardware);
    note(changed, "input_deactivated", existing->input_deactivated, fresh.input_deactivated);
    note(changed, "away", existing->away, fresh.away);
    note(changed, "away_message", existing->away_message, fresh.away_message);
    note(changed, "recording", existing->recording, fresh.recording);
    note(changed, "channel_commander", existing->channel_commander, fresh.channel_commander);
    note(changed, "priority_speaker", existing->priority_speaker, fresh.priority_speaker);
    note(changed, "is_talker", existing->is_talker, fresh.is_talker);
    note(changed, "has_avatar", existing->has_avatar, fresh.has_avatar);
    note(changed, "locally_muted", existing->locally_muted, fresh.locally_muted);
    note(changed, "talk_power", existing->talk_power, fresh.talk_power);
    note(changed, "country", existing->country, fresh.country);

    if (changed.empty()) return changed;

    // Talking is owned by the talk-status callback, which is both more frequent and more
    // authoritative than a property re-read.
    fresh.talking = existing->talking;
    fresh.whispering_to_me = existing->whispering_to_me;
    *existing = std::move(fresh);
    refresh_snapshot_cache();
    return changed;
}

void TsState::set_connection(std::uint64_t server, ConnectionState state) {
    state_.server.handler_id = server;
    state_.server.connection = state;
    if (state != ConnectionState::Connected) {
        state_.users.clear();
        state_.channel = ChannelState{};
        state_.synchronised = false;
    }
    refresh_snapshot_cache();
}

bool TsState::set_talking(std::uint16_t client, bool talking, bool whisper) {
    UserState* u = find_mutable(client);
    if (u == nullptr) return false;
    const bool was_talking = u->talking;
    const bool was_whisper = u->whispering_to_me;
    u->talking = talking;
    u->whispering_to_me = talking && whisper;
    if (was_talking == u->talking && was_whisper == u->whispering_to_me) return false;
    refresh_snapshot_cache();
    return true;
}

bool TsState::add_user(const UserState& user) {
    if (UserState* existing = find_mutable(user.client_id)) {
        *existing = user;
        refresh_snapshot_cache();
        return false;  // already present: an update, not a join
    }
    if (state_.users.size() >= proto::kMaxUsersPerSnapshot) return false;
    state_.users.push_back(user);
    refresh_snapshot_cache();
    return true;
}

bool TsState::remove_user(std::uint16_t client, UserState& removed) {
    const auto it = std::find_if(state_.users.begin(), state_.users.end(),
                                 [&](const UserState& u) { return u.client_id == client; });
    if (it == state_.users.end()) return false;
    removed = *it;
    state_.users.erase(it);
    refresh_snapshot_cache();
    return true;
}

void TsState::clear() {
    state_ = OverlayState{};
    refresh_snapshot_cache();
}

void TsState::refresh_snapshot_cache() {
    json::Value encoded = encode_snapshot(state_);
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_cache_ = std::move(encoded);
}

json::Value TsState::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_cache_;
}

}  // namespace tsro::plugin
