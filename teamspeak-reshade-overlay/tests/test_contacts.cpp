// SPDX-License-Identifier: MIT
#include <filesystem>
#include <string>
#include <vector>

#include "tsro/sqlite_read.hpp"
#include "tsro/ts_contacts.hpp"
#include "tsro_test.hpp"

using namespace tsro;

namespace {

/// The fixtures are real databases, written by SQLite itself rather than hand-assembled, so
/// these tests exercise the actual file format and not our idea of it.
std::string fixture(const char* name) {
    return std::string(TSRO_TEST_DATA) + "/" + name;
}

const Contact* find(const std::vector<Contact>& contacts, const std::string& id) {
    for (const Contact& c : contacts) {
        if (c.unique_id == id) return &c;
    }
    return nullptr;
}

}  // namespace

TEST(sqlite, a_plain_database_yields_every_row) {
    std::vector<sqlite::Row> rows;
    std::string error;
    CHECK(sqlite::read_table(fixture("contacts_plain.db"), "Contacts", rows, error));
    CHECK_EQ(error, std::string());
    CHECK_EQ(rows.size(), static_cast<std::size_t>(4));
    CHECK_EQ(rows[0].size(), static_cast<std::size_t>(2));
    CHECK_EQ(rows[0][0], std::string("Contact0"));
}

TEST(sqlite, a_payload_too_large_for_one_page_is_reassembled) {
    std::vector<sqlite::Row> rows;
    std::string error;
    CHECK(sqlite::read_table(fixture("contacts_plain.db"), "Contacts", rows, error));
    // The fourth contact carries 9 KB of padding, which SQLite spills onto overflow pages.
    bool found = false;
    for (const sqlite::Row& row : rows) {
        if (row.size() > 1 && row[1].find("Overflow") != std::string::npos) {
            CHECK(row[1].size() > 9000);
            found = true;
        }
    }
    CHECK(found);
}

TEST(sqlite, rows_still_in_the_write_ahead_log_are_read) {
    // This fixture's .db holds only a header page: everything is in the uncheckpointed -wal,
    // which is the state a *running* TeamSpeak leaves its settings in. A reader that ignored
    // the log would report an empty contact list and quietly lose every friend.
    std::vector<sqlite::Row> rows;
    std::string error;
    CHECK(sqlite::read_table(fixture("contacts_wal.db"), "Contacts", rows, error));
    CHECK_EQ(error, std::string());
    CHECK_EQ(rows.size(), static_cast<std::size_t>(5));
}

TEST(sqlite, a_missing_table_is_empty_rather_than_an_error) {
    std::vector<sqlite::Row> rows;
    std::string error;
    CHECK(sqlite::read_table(fixture("contacts_plain.db"), "NoSuchTable", rows, error));
    CHECK(rows.empty());
    CHECK_EQ(error, std::string());
}

TEST(sqlite, a_file_that_is_not_a_database_is_reported) {
    std::vector<sqlite::Row> rows;
    std::string error;
    CHECK(!sqlite::read_table(fixture("does-not-exist.db"), "Contacts", rows, error));
    CHECK(!error.empty());
}

TEST(contacts, teamspeak_friends_are_read_with_their_nicknames) {
    std::vector<sqlite::Row> rows;
    std::string error;
    CHECK(sqlite::read_table(fixture("contacts_plain.db"), "Contacts", rows, error));
    const std::vector<Contact> contacts = parse_contacts(rows);
    CHECK_EQ(contacts.size(), static_cast<std::size_t>(4));

    const Contact* chief = find(contacts, "aaaaBBBBccccDDDD=");
    CHECK(chief != nullptr);
    CHECK(chief->kind == ContactKind::Friend);
    CHECK_EQ(chief->nickname, std::string("Chief"));
}

TEST(contacts, neutral_and_blocked_contacts_are_distinguished) {
    std::vector<sqlite::Row> rows;
    std::string error;
    CHECK(sqlite::read_table(fixture("contacts_plain.db"), "Contacts", rows, error));
    const std::vector<Contact> contacts = parse_contacts(rows);

    const Contact* neutral = find(contacts, "eeeeFFFFgggg=");
    CHECK(neutral != nullptr);
    CHECK(neutral->kind == ContactKind::Neutral);
    CHECK(neutral->nickname.empty());

    const Contact* blocked = find(contacts, "hhhhIIIIjjjj=");
    CHECK(blocked != nullptr);
    CHECK(blocked->kind == ContactKind::Blocked);
}

TEST(contacts, a_row_without_an_identity_is_skipped) {
    std::vector<sqlite::Row> rows = {{"key", "Nickname=Nobody\nFriend=2\n"}, {"key", ""}};
    CHECK(parse_contacts(rows).empty());
}

TEST(contacts, the_column_carrying_the_blob_is_found_wherever_it_sits) {
    // The blob is located by content rather than by column index, so a client update that adds
    // a column cannot silently empty the friend list.
    const std::vector<sqlite::Row> rows = {
        {"1", "2026-01-01", "IDENTITY=zzz=\nNickname=Zed\nFriend=0\n", "trailing"}};
    const std::vector<Contact> contacts = parse_contacts(rows);
    CHECK_EQ(contacts.size(), static_cast<std::size_t>(1));
    CHECK_EQ(contacts[0].nickname, std::string("Zed"));
}

TEST(contacts, windows_line_endings_are_tolerated) {
    const std::vector<sqlite::Row> rows = {
        {"1", "IDENTITY=crlf=\r\nNickname=Carl\r\nFriend=2\r\n"}};
    const std::vector<Contact> contacts = parse_contacts(rows);
    CHECK_EQ(contacts.size(), static_cast<std::size_t>(1));
    CHECK_EQ(contacts[0].unique_id, std::string("crlf="));
    CHECK_EQ(contacts[0].nickname, std::string("Carl"));
    CHECK(contacts[0].kind == ContactKind::Friend);
}

TEST(contacts, the_friend_value_matches_the_clients_own_ordering) {
    // Pinned against what the TeamSpeak client actually stores, checked against its Contacts
    // dialog: the three states are listed Neutral, Blocked, Friend, and stored 0, 1, 2. The
    // first guess here was 0 for Friend, which made every real friend read as blocked and left
    // the overlay with no friends at all.
    const std::vector<sqlite::Row> rows = {
        {"0", "IDENTITY=neutral=\nFriend=0\n"},
        {"1", "IDENTITY=blocked=\nFriend=1\n"},
        {"2", "IDENTITY=friend=\nFriend=2\n"},
    };
    const std::vector<Contact> contacts = parse_contacts(rows);
    CHECK_EQ(contacts.size(), static_cast<std::size_t>(3));
    CHECK(find(contacts, "neutral=")->kind == ContactKind::Neutral);
    CHECK(find(contacts, "blocked=")->kind == ContactKind::Blocked);
    CHECK(find(contacts, "friend=")->kind == ContactKind::Friend);
}

TEST(contacts, the_raw_value_is_carried_so_a_wrong_mapping_is_visible) {
    const std::vector<sqlite::Row> rows = {{"0", "IDENTITY=a=\nFriend=2\n"},
                                           {"1", "IDENTITY=b=\nNickname=NoFlag\n"}};
    const std::vector<Contact> contacts = parse_contacts(rows);
    CHECK_EQ(find(contacts, "a=")->raw_flag, 2);
    CHECK_EQ(find(contacts, "b=")->raw_flag, -1);
}

TEST(contacts, a_contact_is_found_by_its_identity_whatever_the_field_is_called) {
    // The last resort, and the only part of this that assumes nothing: the unique identifier is
    // a long distinctive string, so a stored value containing it is that person's entry even
    // when every field name is one this code has never seen.
    const std::string blob =
        "SomeUnexpectedKey=aaaaBBBBccccDDDD=\nContactName=drew\nFriend=2\nVolume=0\n";
    Contact c;
    CHECK(contact_from_blob(blob, "aaaaBBBBccccDDDD=", c));
    CHECK(c.kind == ContactKind::Friend);
    CHECK_EQ(c.raw_flag, 2);
}

TEST(contacts, a_blob_that_does_not_mention_them_is_not_their_entry) {
    Contact c;
    CHECK(!contact_from_blob("IDENTITY=someone-else=\nFriend=2\n", "aaaa=", c));
}

TEST(contacts, candidate_values_are_kept_even_when_the_structured_parse_finds_nothing) {
    const std::vector<sqlite::Row> rows = {
        {"0", "Unrecognised=aaaaBBBBccccDDDD=\nFriend=2\nNickname=drew\n"}};
    CHECK(parse_contacts(rows).empty());          // no key it recognises as an identity
    CHECK_EQ(contact_blobs(rows).size(), static_cast<std::size_t>(1));
}
