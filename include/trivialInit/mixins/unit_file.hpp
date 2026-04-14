#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace tinit {

enum class UnitType : uint8_t {
    Service,
    Target,
    Mount,
    Socket,
    Unknown,
};

inline UnitType unit_type_from_suffix(std::string_view path) {
    if (path.ends_with(".service")) return UnitType::Service;
    if (path.ends_with(".target"))  return UnitType::Target;
    if (path.ends_with(".mount"))   return UnitType::Mount;
    if (path.ends_with(".socket"))  return UnitType::Socket;
    return UnitType::Unknown;
}

/// Parsed representation of a systemd-compatible unit file
struct UnitFile {
    std::string name;
    std::string path;
    UnitType    type = UnitType::Unknown;

    // [Unit]
    std::string              description;
    std::vector<std::string> wants;
    std::vector<std::string> requires_;
    std::vector<std::string> after;
    std::vector<std::string> before;
    std::vector<std::string> conflicts;
    std::vector<std::string> part_of;       // PartOf=
    std::vector<std::string> binds_to;      // BindsTo=

    // [Service]
    std::string exec_start;
    std::string exec_stop;
    std::string exec_reload;                // ExecReload=
    std::vector<std::string> exec_start_pre;
    std::vector<std::string> exec_start_post;
    std::string type_str;                   // Type= (simple, forking, oneshot, notify, dbus, idle)
    std::string restart_policy;             // Restart= (no, on-success, on-failure, on-abnormal, on-watchdog, on-abort, always)
    std::string user;
    std::string group;
    std::string working_directory;
    std::string runtime_directory;          // RuntimeDirectory=
    std::string pid_file;                   // PIDFile= (for forking type)
    std::unordered_map<std::string, std::string> environment;

    // Restart rate-limiting (mirrors systemd defaults: 5 starts / 10 seconds)
    int restart_limit_burst    = 5;         // StartLimitBurst=
    int restart_limit_interval = 10;        // StartLimitIntervalSec=

    // [Install]
    std::vector<std::string> wanted_by;
    std::vector<std::string> required_by;
    std::string alias;

    // [Mount]
    std::string what;
    std::string where;
    std::string mount_type;
    std::string options;
    bool        lazy_unmount = false;       // LazyUnmount=

    // Raw storage for anything not explicitly parsed
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> raw_sections;

    bool is_enabled() const {
        return !wanted_by.empty() || !required_by.empty();
    }
};

} // namespace tinit