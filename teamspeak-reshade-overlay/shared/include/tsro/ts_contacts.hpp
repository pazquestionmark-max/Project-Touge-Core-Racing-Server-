// SPDX-License-Identifier: MIT
// TeamSpeak's own contact list.
//
// The plugin API has no friend, buddy or contact call -- grep the whole SDK and the word appears
// only in a comment about the client having already filtered a message. The list the client's
// Contacts dialog edits lives in its settings.db, keyed by the same unique identity the overlay
// already uses for everything else, so that is where this reads it from.
//
// Read-only, and never while holding a lock: a stale read costs a refresh, a write would cost
// the user their contacts.
#ifndef TSRO_TS_CONTACTS_HPP
#define TSRO_TS_CONTACTS_HPP

#include <string>
#include <vector>

#include "tsro/sqlite_read.hpp"

namespace tsro {

/// TeamSpeak's three contact states.
enum class ContactKind { Neutral, Friend, Blocked };

struct Contact {
    std::string unique_id;   ///< CLIENT_UNIQUE_IDENTIFIER, the same key used everywhere else
    std::string nickname;    ///< the name the user gave them in TeamSpeak, if any
    ContactKind kind = ContactKind::Neutral;
    /// The `Friend=` value exactly as the client wrote it, or -1 when the entry had none.
    int raw_flag = -1;
};

/// Parses the rows of the client's `Contacts` table.
///
/// The table stores one INI-like blob per contact rather than a column per field, so this scans
/// each row's columns for the one carrying an IDENTITY line instead of assuming a column order
/// that a client update could change.
std::vector<Contact> parse_contacts(const std::vector<sqlite::Row>& rows);

/// What a read of the contact list actually did, so a failure says which step failed rather
/// than just producing no friends.
struct ContactReadReport {
    std::string path;          ///< the database that was opened
    std::string table;         ///< the table the contacts were found in, if any
    std::size_t tables = 0;    ///< how many tables the database holds
    std::size_t rows = 0;      ///< rows in the table that was read
    std::size_t parsed = 0;    ///< rows that yielded an identity
    std::string summary() const;
};

/// Reads `<config_dir>/settings.db`. A missing file or table yields no contacts and no error:
/// a user who has never added one is not a failure.
///
/// The table is located rather than assumed: the expected name is tried first, without regard to
/// case, and every other table is tried after it. A client that renames or restructures its
/// settings should cost a slower search, not a silently empty friend list.
bool read_contacts(const std::string& config_dir, std::vector<Contact>& out, std::string& error,
                   ContactReadReport* report = nullptr);

}  // namespace tsro

#endif  // TSRO_TS_CONTACTS_HPP
