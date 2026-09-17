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

/// TeamSpeak stores the contact state as a small integer.
///
/// 0 is a friend, 1 is blocked and anything else is neutral. The overlay only ever *reads* this,
/// so a client that renumbered them would cost a wrong colour, never a wrong write -- and the
/// diagnostics panel reports the counts so a mismatch is visible rather than silent.
ContactKind kind_from(std::string_view value) {
    if (value == "0") return ContactKind::Friend;
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
