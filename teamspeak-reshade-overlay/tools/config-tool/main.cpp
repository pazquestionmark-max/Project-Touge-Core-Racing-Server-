// SPDX-License-Identifier: MIT
// tsro-config — the C++ side of the configuration tooling.
//
// Exists for three jobs: emitting the authoritative defaults (used to generate the shipped
// example profile), validating a file with the same code the add-on uses, and printing the
// ranges the C++ validator enforces so the TypeScript implementation can be checked against it.
// Without that last one the two schema implementations could drift apart silently.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "tsro/config.hpp"
#include "tsro/profile_store.hpp"

namespace {

int usage() {
    std::cout <<
        "tsro-config — configuration tooling (C++ side)\n\n"
        "Usage:\n"
        "  tsro-config defaults              Print the built-in defaults as JSON\n"
        "  tsro-config validate <file...>    Validate files, reporting every repair\n"
        "  tsro-config normalise <file>      Print the file as the loader understands it\n"
        "  tsro-config probe                 Print each field's accepted range, as JSON\n"
        "\nExit codes: 0 ok, 1 problems found, 2 usage error.\n";
    return 2;
}

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = true;
    return buffer.str();
}

const char* severity_name(tsro::ConfigIssue::Severity severity) {
    switch (severity) {
        case tsro::ConfigIssue::Severity::Error: return "error  ";
        case tsro::ConfigIssue::Severity::Warning: return "warning";
        case tsro::ConfigIssue::Severity::Info: return "note   ";
    }
    return "note   ";
}

int report(const std::string& path, const tsro::ConfigDiagnostics& diagnostics) {
    if (diagnostics.issues.empty()) {
        std::cout << path << ": ok\n";
        return 0;
    }
    std::cout << path << ":\n";
    int worst = 0;
    for (const tsro::ConfigIssue& issue : diagnostics.issues) {
        std::cout << "  " << severity_name(issue.severity)
                  << (issue.path.empty() ? "" : " ") << issue.path
                  << (issue.path.empty() ? "" : ":") << " " << issue.message << "\n";
        if (issue.severity == tsro::ConfigIssue::Severity::Error) worst = 1;
    }
    return worst;
}

/// Probes the validator empirically rather than duplicating the range table: for each field we
/// feed an absurdly small and an absurdly large value and report what came back. That way the
/// output is the behaviour, not a second copy of the rules that could itself be wrong.
void probe_numeric(tsro::json::Value& out, const char* section, const char* field,
                   double low_probe, double high_probe,
                   double (*read)(const tsro::Config&)) {
    const auto measure = [&](double value) {
        tsro::json::Value document{tsro::json::Object{}};
        document.set("config_version", tsro::json::Value(tsro::kConfigVersion));
        tsro::json::Value group{tsro::json::Object{}};
        group.set(field, tsro::json::Value(value));
        document.set(section, std::move(group));
        tsro::ConfigDiagnostics diagnostics;
        return read(tsro::Config::from_json(document, diagnostics));
    };
    tsro::json::Value range{tsro::json::Object{}};
    range.set("min", tsro::json::Value(measure(low_probe)));
    range.set("max", tsro::json::Value(measure(high_probe)));
    out.set(std::string(section) + "." + field, std::move(range));
}

int command_probe() {
    tsro::json::Value out{tsro::json::Object{}};
    out.set("config_version", tsro::json::Value(tsro::kConfigVersion));

    tsro::json::Value ranges{tsro::json::Object{}};
    probe_numeric(ranges, "general", "master_opacity", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.general.master_opacity); });
    probe_numeric(ranges, "general", "scale", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.general.scale); });
    probe_numeric(ranges, "appearance", "font_size", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.appearance.font_size); });
    probe_numeric(ranges, "appearance", "icon_size", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.appearance.icon_size); });
    probe_numeric(ranges, "appearance", "row_height", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.appearance.row_height); });
    probe_numeric(ranges, "appearance", "corner_radius", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.appearance.corner_radius); });
    probe_numeric(ranges, "user_list", "max_visible_users", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.user_list.max_visible_users); });
    probe_numeric(ranges, "user_list", "max_name_width", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.user_list.max_name_width); });
    probe_numeric(ranges, "user_list", "min_font_scale", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.user_list.min_font_scale); });
    probe_numeric(ranges, "notifications", "max_visible", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.notifications.max_visible); });
    probe_numeric(ranges, "notifications", "width", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.notifications.width); });
    probe_numeric(ranges, "chat", "history_size", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.chat.history_size); });
    probe_numeric(ranges, "chat", "max_message_length", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.chat.max_message_length); });
    probe_numeric(ranges, "chat", "retention_seconds", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.chat.retention_seconds); });
    probe_numeric(ranges, "animation", "speaking_attack_ms", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.animation.speaking_attack_ms); });
    probe_numeric(ranges, "animation", "speaking_pulse_hz", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.animation.speaking_pulse_hz); });
    probe_numeric(ranges, "animation", "idle_opacity", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.animation.idle_opacity); });
    probe_numeric(ranges, "integration", "reconnect_initial_ms", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.integration.reconnect_initial_ms); });
    probe_numeric(ranges, "integration", "stale_after_ms", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.integration.stale_after_ms); });
    probe_numeric(ranges, "integration", "ping_interval_ms", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.integration.ping_interval_ms); });
    probe_numeric(ranges, "logging", "max_file_kb", -1e9, 1e9,
                  [](const tsro::Config& c) { return static_cast<double>(c.logging.max_file_kb); });
    out.set("ranges", std::move(ranges));

    // The protocol constants the TypeScript side mirrors.
    tsro::json::Value limits{tsro::json::Object{}};
    limits.set("max_message_bytes", tsro::json::Value(65536));
    limits.set("max_chat_chars", tsro::json::Value(1024));
    limits.set("max_users_per_snapshot", tsro::json::Value(512));
    out.set("limits", std::move(limits));

    std::cout << out.dump() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string command = argv[1];

    if (command == "defaults") {
        std::cout << tsro::Config::defaults().serialise() << "\n";
        return 0;
    }
    if (command == "probe") return command_probe();

    if (command == "validate") {
        if (argc < 3) return usage();
        int worst = 0;
        for (int i = 2; i < argc; ++i) {
            bool ok = false;
            const std::string text = read_file(argv[i], ok);
            if (!ok) {
                std::cerr << argv[i] << ": cannot read\n";
                worst = 1;
                continue;
            }
            tsro::ConfigDiagnostics diagnostics;
            const tsro::Config config = tsro::Config::parse(text, diagnostics);
            (void)config;
            worst = std::max(worst, report(argv[i], diagnostics));
            if (diagnostics.from_defaults) worst = 1;
        }
        return worst;
    }

    if (command == "normalise") {
        if (argc < 3) return usage();
        bool ok = false;
        const std::string text = read_file(argv[2], ok);
        if (!ok) {
            std::cerr << argv[2] << ": cannot read\n";
            return 1;
        }
        tsro::ConfigDiagnostics diagnostics;
        std::cout << tsro::Config::parse(text, diagnostics).serialise() << "\n";
        return diagnostics.from_defaults ? 1 : 0;
    }

    return usage();
}
