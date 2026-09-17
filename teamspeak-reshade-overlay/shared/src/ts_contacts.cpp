// SPDX-License-Identifier: MIT
#include "tsro/ts_contacts.hpp"

#include <algorithm>

namespace tsro {
namespace {

/// Splits "Key=Value" out of one line, tolerating CR line endings and surrounding blanks.
bool split_setting(std::string_view line, std::string_view& key, std::string_view& value) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) return false;
    key = line.substr(0, eq);
    value = line.substr(eq + 1);
    return !key.empty();
}

/// TeamSpeak stores the contact state as a small integer, in the order its own Contacts dialog
/// lists the three options: Neutral, Blocked, Friend.
///
/// This was guessed the other way round first, which made every real friend read as blocked and
/// nobody read as a friend at all. The overlay only ever *reads* this value, so the cost of the
/// wrong guess was a missing colour rather than damage -- but it is a guess no longer: the
/// Diagnostics panel prints the raw number per person, and user_list.teamspeak_friend_value
/// overrides the mapping for a client that ever renumbers them.
ContactKind kind_from(std::string_view value) {
    if (value == "2") return ContactKind::Friend;
    if (value == "1") return ContactKind::Blocked;
    return ContactKind::Neutral;
}

}  // namespace

std::vector<Contact> parse_contacts(const std::vector<sqlite::Row>& rows) {
    std::vector<Contact> out;
    out.reserve(rows.size());

    for (const sqlite::Row& row : rows) {
        for (const std::string& column : row) {
            if (column.find("IDENTITY=") == std::string::npos) continue;

            Contact contact;
            bool has_flag = false;
            std::size_t at = 0;
            while (at <= column.size()) {
                const std::size_t nl = column.find('\n', at);
                const std::string_view line(column.data() + at,
                                            (nl == std::string::npos ? column.size() : nl) - at);
                at = nl == std::string::npos ? column.size() + 1 : nl + 1;

                std::string_view key;
                std::string_view value;
                if (!split_setting(line, key, value)) continue;
                if (key == "IDENTITY") {
                    contact.unique_id.assign(value);
                } else if (key == "Nickname") {
                    contact.nickname.assign(value);
                } else if (key == "Friend") {
                    contact.kind = kind_from(value);
                    contact.raw_flag = 0;
                    for (const char c : value) {
                        if (c < '0' || c > '9') {
                            contact.raw_flag = -1;
                            break;
                        }
                        contact.raw_flag = contact.raw_flag * 10 + (c - '0');
                    }
                    if (value.empty()) contact.raw_flag = -1;
                    has_flag = true;
                }
            }
            (void)has_flag;
            if (!contact.unique_id.empty()) out.push_back(std::move(contact));
            break;   // one identity per row
        }
    }

    // Later rows win, so a duplicated identity resolves to the most recent entry.
    std::stable_sort(out.begin(), out.end(),
                     [](const Contact& l, const Contact& r) { return l.unique_id < r.unique_id; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Contact& l, const Contact& r) {
                              return l.unique_id == r.unique_id;
                          }),
              out.end());
    return out;
}

bool read_contacts(const std::string& config_dir, std::vector<Contact>& out, std::string& error) {
    out.clear();
    error.clear();
    if (config_dir.empty()) {
        error = "TeamSpeak did not report a configuration folder";
        return false;
    }
    std::string path = config_dir;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    path += "settings.db";

    std::vector<sqlite::Row> rows;
    if (!sqlite::read_table(path, "Contacts", rows, error)) return false;
    out = parse_contacts(rows);
    return true;
}

}  // namespace tsro
