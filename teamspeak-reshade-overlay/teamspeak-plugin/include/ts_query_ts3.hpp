// SPDX-License-Identifier: MIT
#ifndef TSRO_TS_QUERY_TS3_HPP
#define TSRO_TS_QUERY_TS3_HPP

#include "ts_state.hpp"

struct TS3Functions;

namespace tsro::plugin {

/// TsQuery backed by the real TeamSpeak client. Holds the function-pointer table TeamSpeak
/// handed us in ts3plugin_setFunctionPointers; it is valid for the plugin's whole lifetime.
class Ts3Query final : public TsQuery {
public:
    explicit Ts3Query(const TS3Functions& functions) : ts_(functions) {}

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

private:
    const TS3Functions& ts_;
};

}  // namespace tsro::plugin

#endif  // TSRO_TS_QUERY_TS3_HPP
