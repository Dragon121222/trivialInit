#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace tinit {

/// Subset of systemd unit types we support
enum class UnitType : uint8_t {
    Service,
    Target,
    Mount,
    Unknown,
};

inline UnitType unit_type_from_suffix(std::string_view path) {
    if (path.ends_with(".service")) return UnitType::Service;
    if (path.ends_with(".target"))  return UnitType::Target;
    if (path.ends_with(".mount"))   return UnitType::Mount;
    return UnitType::Unknown;
}

/// Parsed representation of a systemd-compatible unit file
struct UnitFile {
    std::string name;          // e.g. "sshd.service"
    std::string path;          // full path on disk
    UnitType type = UnitType::Unknown;

    // [Unit] section
    std::string description;
    std::vector<std::string> wants;           // Wants=
    std::vector<std::string> requires_;       // Requires=
    std::vector<std::string> after;           // After=
    std::vector<std::string> before;          // Before=
    std::vector<std::string> conflicts;       // Conflicts=

    // [Service] section (only for .service)
    std::string exec_start;                   // ExecStart=
    std::string exec_stop;                    // ExecStop=
    std::vector<std::string> exec_start_pre;  // ExecStartPre=
    std::string type_str;                     // Type= (simple, forking, oneshot)
    std::string restart_policy;               // Restart= (no, on-failure, always)
    std::string user;                         // User=
    std::string group;                        // Group=
    std::string working_directory;            // WorkingDirectory=
    std::unordered_map<std::string, std::string> environment; // Environment=

    // [Install] section
    std::vector<std::string> wanted_by;       // WantedBy=
    std::vector<std::string> required_by;     // RequiredBy=
    std::string alias;                        // Alias=

    // [Mount] section (only for .mount)
    std::string what;          // What=
    std::string where;         // Where=
    std::string mount_type;    // Type=
    std::string options;       // Options=

    // Raw storage for anything we don't parse
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> raw_sections;

    bool is_enabled() const {
        return !wanted_by.empty() || !required_by.empty();
    }
};

} // namespace tinit
