// SPDX-License-Identifier: MIT
// TeamSpeak state management: converts the client's vocabulary into the normalised model and
// works out what actually changed.
//
// Everything here runs on the TeamSpeak callback thread and never performs I/O. The only
// cross-thread surface is the snapshot cache, which the IPC threads read under a mutex.
//
// The query interface is expressed in *this project's* property enum rather than TeamSpeak's
// integer flags. The adapter in ts_query_ts3.cpp maps them onto the real SDK enumerators, which
// means (a) no SDK constant is ever hardcoded here, and (b) the state machine is testable
// against a fake client without linking TeamSpeak at all.
#ifndef TSRO_TS_STATE_HPP
#define TSRO_TS_STATE_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "tsro/model.hpp"
#include "tsro/ts_contacts.hpp"

namespace tsro::plugin {

/// Integer-valued client properties this plugin reads. Each maps to a documented
/// ClientProperties / ClientPropertiesRare enumerator; see docs/protocol.md §6.
enum class UserFlag {
    InputMuted,        ///< CLIENT_INPUT_MUTED
    OutputMuted,       ///< CLIENT_OUTPUT_MUTED — speakers off; the SDK says this implies mic mute
    OutputOnlyMuted,   ///< CLIENT_OUTPUTONLY_MUTED — speakers off while the mic may still be live
    InputHardware,     ///< CLIENT_INPUT_HARDWARE
    OutputHardware,    ///< CLIENT_OUTPUT_HARDWARE
    InputDeactivated,  ///< CLIENT_INPUT_DEACTIVATED — own client only, per the SDK
    Away,              ///< CLIENT_AWAY
    Recording,         ///< CLIENT_IS_RECORDING
    ChannelCommander,  ///< CLIENT_IS_CHANNEL_COMMANDER
    PrioritySpeaker,   ///< CLIENT_IS_PRIORITY_SPEAKER
    IsTalker,          ///< CLIENT_IS_TALKER
    HasAvatar,         ///< CLIENT_FLAG_AVATAR — a flag only; the image is not obtainable
    LocallyMuted,      ///< CLIENT_IS_MUTED
    TalkPower,         ///< CLIENT_TALK_POWER
};

/// String-valued client properties.
enum class UserText {
    UniqueIdentifier,  ///< CLIENT_UNIQUE_IDENTIFIER — the identity key
    Nickname,          ///< CLIENT_NICKNAME
    AwayMessage,       ///< CLIENT_AWAY_MESSAGE
    Country,           ///< CLIENT_COUNTRY
};

enum class ChannelText { Name, Topic };
enum class ServerText { Name, UniqueIdentifier, ClientVersion };

/// Thin, testable wrapper over the TeamSpeak query functions.
class TsQuery {
public:
    virtual ~TsQuery() = default;

    /// Each getter returns false when the property is unavailable for that client. False means
    /// "unknown", and the caller must leave the model field absent rather than defaulting it.
    virtual bool user_flag(std::uint64_t server, std::uint16_t client, UserFlag flag,
                           int& out) = 0;
    virtual bool user_text(std::uint64_t server, std::uint16_t client, UserText field,
                           std::string& out) = 0;
    virtual bool self_flag(std::uint64_t server, UserFlag flag, int& out) = 0;
    virtual bool channel_text(std::uint64_t server, std::uint64_t channel, ChannelText field,
                              std::string& out) = 0;
    virtual bool server_text(std::uint64_t server, ServerText field, std::string& out) = 0;
    virtual bool own_client_id(std::uint64_t server, std::uint16_t& out) = 0;
    virtual bool channel_of_client(std::uint64_t server, std::uint16_t client,
                                   std::uint64_t& out) = 0;
    virtual bool parent_of_channel(std::uint64_t server, std::uint64_t channel,
                                   std::uint64_t& out) = 0;
    virtual bool channel_clients(std::uint64_t server, std::uint64_t channel,
                                 std::vector<std::uint16_t>& out) = 0;
    virtual bool display_name(std::uint64_t server, std::uint16_t client, std::string& out) = 0;
};

/// The plugin's view of the connection the local user is currently on.
class TsState {
public:
    explicit TsState(TsQuery& query) : query_(query) {}

    /// Rebuilds everything for `server` from the client. Called on connect, on channel change,
    /// and once a channel subscription batch completes.
    void resynchronise(std::uint64_t server);

    /// Re-reads one user. Returns the names of the fields that changed, which is exactly what
    /// `user_updated.changed` carries; empty means nothing worth sending happened.
    std::vector<std::string> refresh_user(std::uint64_t server, std::uint16_t client);

    bool read_user(std::uint64_t server, std::uint16_t client, UserState& out) const;
    bool read_channel(std::uint64_t server, std::uint64_t channel, ChannelState& out) const;

    /// Replaces the contact list read from the client's own settings, and re-stamps everyone
    /// already on screen so a change in TeamSpeak's Contacts dialog shows up without a rejoin.
    void set_contacts(std::vector<Contact> contacts, std::vector<std::string> blobs = {});
    std::size_t contact_count() const noexcept { return contacts_.size(); }
    std::size_t friend_count() const noexcept;

    void set_connection(std::uint64_t server, ConnectionState state);
    /// Returns true when the talking or whisper state actually changed.
    bool set_talking(std::uint16_t client, bool talking, bool whisper);
    bool add_user(const UserState& user);
    bool remove_user(std::uint16_t client, UserState& removed);
    void clear();

    const OverlayState& state() const noexcept { return state_; }
    std::uint64_t server_handler() const noexcept { return state_.server.handler_id; }
    std::uint64_t channel_id() const noexcept { return state_.channel.id; }
    const std::string& server_uid() const noexcept { return state_.server.unique_id; }
    bool has_user(std::uint16_t client) const { return find(client) != nullptr; }
    const UserState* find(std::uint16_t client) const;

    /// Snapshot cache, refreshed on the TeamSpeak thread and read by the IPC threads. This is
    /// what keeps the IPC layer from ever calling back into TeamSpeak.
    void refresh_snapshot_cache();
    json::Value snapshot() const;

private:
    UserState* find_mutable(std::uint16_t client);
    /// Applies the contact list to one user. Leaves the fields absent when no list has been
    /// read: "we do not know" and "not a friend" must not look the same to the overlay.
    void stamp_contact(UserState& user) const;

    TsQuery& query_;
    /// Mutable from the const stamp: an identity resolved out of a raw value is cached here
    /// so the scan happens once per person, not once per read of their row.
    mutable std::map<std::string, Contact> contacts_;
    std::vector<std::string> contact_blobs_;
    bool contacts_known_ = false;
    OverlayState state_;
    mutable std::mutex snapshot_mutex_;
    json::Value snapshot_cache_{json::Object{}};
};

/// Capability strings this build can genuinely supply, sent in `hello`. Anything absent here is
/// something the overlay must not render an indicator for.
std::vector<std::string> supported_capabilities();

}  // namespace tsro::plugin

#endif  // TSRO_TS_STATE_HPP
