// SPDX-License-Identifier: MIT
// A TsQuery backed by an in-memory model instead of a live TeamSpeak client.
//
// This is what makes the plugin's event handling testable: TsState and PluginCore are exercised
// against a scripted client, on any platform, with no TeamSpeak installed. It is compiled into
// the test binary only and never into the shipped plugin.
#include "ts_query_stub.hpp"

#include <algorithm>

namespace tsro::plugin {

bool FakeTsQuery::user_flag(std::uint64_t server, std::uint16_t client, UserFlag flag,
                            int& out) {
    const FakeClient* c = find(server, client);
    if (c == nullptr) return false;
    const auto it = c->flags.find(flag);
    if (it == c->flags.end()) return false;  // unavailable: the model field must stay absent
    out = it->second;
    return true;
}

bool FakeTsQuery::user_text(std::uint64_t server, std::uint16_t client, UserText field,
                           std::string& out) {
    const FakeClient* c = find(server, client);
    if (c == nullptr) return false;
    const auto it = c->text.find(field);
    if (it == c->text.end()) return false;
    out = it->second;
    return true;
}

bool FakeTsQuery::self_flag(std::uint64_t server, UserFlag flag, int& out) {
    const FakeServer* s = find(server);
    if (s == nullptr) return false;
    return user_flag(server, s->own_client_id, flag, out);
}

bool FakeTsQuery::channel_text(std::uint64_t server, std::uint64_t channel, ChannelText field,
                               std::string& out) {
    const FakeServer* s = find(server);
    if (s == nullptr) return false;
    const auto it = s->channels.find(channel);
    if (it == s->channels.end()) return false;
    out = field == ChannelText::Name ? it->second.name : it->second.topic;
    return true;
}

bool FakeTsQuery::server_text(std::uint64_t server, ServerText field, std::string& out) {
    const FakeServer* s = find(server);
    if (s == nullptr) return false;
    switch (field) {
        case ServerText::Name: out = s->name; return true;
        case ServerText::UniqueIdentifier: out = s->unique_id; return true;
        case ServerText::ClientVersion: out = s->version; return true;
    }
    return false;
}

bool FakeTsQuery::own_client_id(std::uint64_t server, std::uint16_t& out) {
    const FakeServer* s = find(server);
    if (s == nullptr || s->own_client_id == 0) return false;
    out = s->own_client_id;
    return true;
}

bool FakeTsQuery::channel_of_client(std::uint64_t server, std::uint16_t client,
                                    std::uint64_t& out) {
    const FakeClient* c = find(server, client);
    if (c == nullptr) return false;
    out = c->channel;
    return true;
}

bool FakeTsQuery::parent_of_channel(std::uint64_t server, std::uint64_t channel,
                                    std::uint64_t& out) {
    const FakeServer* s = find(server);
    if (s == nullptr) return false;
    const auto it = s->channels.find(channel);
    if (it == s->channels.end()) return false;
    out = it->second.parent;
    return true;
}

bool FakeTsQuery::channel_clients(std::uint64_t server, std::uint64_t channel,
                                  std::vector<std::uint16_t>& out) {
    out.clear();
    const FakeServer* s = find(server);
    if (s == nullptr) return false;
    for (const auto& [id, client] : s->clients) {
        if (client.channel == channel) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return true;
}

bool FakeTsQuery::display_name(std::uint64_t server, std::uint16_t client, std::string& out) {
    const FakeClient* c = find(server, client);
    if (c == nullptr || c->display_name.empty()) return false;
    out = c->display_name;
    return true;
}

const FakeServer* FakeTsQuery::find(std::uint64_t server) const {
    const auto it = servers.find(server);
    return it == servers.end() ? nullptr : &it->second;
}

const FakeClient* FakeTsQuery::find(std::uint64_t server, std::uint16_t client) const {
    const FakeServer* s = find(server);
    if (s == nullptr) return nullptr;
    const auto it = s->clients.find(client);
    return it == s->clients.end() ? nullptr : &it->second;
}

}  // namespace tsro::plugin
