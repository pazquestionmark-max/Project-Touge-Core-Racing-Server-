// SPDX-License-Identifier: MIT
#ifndef TSRO_TS_QUERY_STUB_HPP
#define TSRO_TS_QUERY_STUB_HPP

#include <map>
#include <string>

#include "ts_state.hpp"

namespace tsro::plugin {

struct FakeChannel {
    std::string name;
    std::string topic;
    std::uint64_t parent = 0;
};

struct FakeClient {
    std::string display_name;
    std::uint64_t channel = 0;
    /// Only the properties present here are reported as available, which is how a test models
    /// "TeamSpeak cannot tell us this for that client".
    std::map<UserFlag, int> flags;
    std::map<UserText, std::string> text;
};

struct FakeServer {
    std::string name;
    std::string unique_id;
    std::string version;
    std::uint16_t own_client_id = 0;
    std::map<std::uint64_t, FakeChannel> channels;
    std::map<std::uint16_t, FakeClient> clients;
};

/// TsQuery over a scripted in-memory client. Test-only.
class FakeTsQuery final : public TsQuery {
public:
    std::map<std::uint64_t, FakeServer> servers;

    bool user_flag(std::uint64_t server, std::uint16_t client, UserFlag flag, int& out) override;
    bool user_text(std::uint64_t server, std::uint16_t client, UserText field,
                   std::string& out) override;
    bool self_flag(std::uint64_t server, UserFlag flag, int& out) override;
    bool channel_text(std::uint64_t server, std::uint64_t channel, ChannelText field,
                      std::string& out) override;
    bool server_text(std::uint64_t server, ServerText field, std::string& out) override;
    bool own_client_id(std::uint64_t server, std::uint16_t& out) override;
    bool channel_of_client(std::uint64_t server, std::uint16_t client,
                           std::uint64_t& out) override;
    bool parent_of_channel(std::uint64_t server, std::uint64_t channel,
                           std::uint64_t& out) override;
    bool channel_clients(std::uint64_t server, std::uint64_t channel,
                         std::vector<std::uint16_t>& out) override;
    bool display_name(std::uint64_t server, std::uint16_t client, std::string& out) override;

    const FakeServer* find(std::uint64_t server) const;
    const FakeClient* find(std::uint64_t server, std::uint16_t client) const;
};

}  // namespace tsro::plugin

#endif  // TSRO_TS_QUERY_STUB_HPP
