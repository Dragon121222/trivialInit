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

/// A single Condition= or Assert= entry.
/// systemd allows negation with a leading '!' meaning "pass if NOT true".
struct ConditionEntry {
    std::string type;   // e.g. "ConditionPathExists", "AssertPathExists"
    std::string value;  // e.g. "/etc/fstab", "!virtualization"
    bool        negate; // true if value originally started with '!'
    bool        is_assert; // true for Assert*, false for Condition*
};

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
    std::vector<std::string> part_of;
    std::vector<std::string> binds_to;

    // Conditions (any false → skip unit silently)
    std::vector<ConditionEntry> conditions;
    // Assertions (any false → fail unit with error)
    std::vector<ConditionEntry> assertions;

    // [Service]
    std::string              exec_start;
    std::string              exec_stop;
    std::string              exec_reload;
    std::vector<std::string> exec_start_pre;
    std::vector<std::string> exec_start_post;
    std::string              type_str;         // simple, forking, oneshot, notify, dbus, idle
    std::string              restart_policy;
    std::string              user;
    std::string              group;
    std::string              working_directory;
    std::string              runtime_directory;
    std::string              pid_file;
    std::unordered_map<std::string, std::string> environment;
    bool                     dynamic_user = false;   // DynamicUser=yes

    int restart_limit_burst    = 5;
    int restart_limit_interval = 10;

    // [Install]
    std::vector<std::string> wanted_by;
    std::vector<std::string> required_by;
    std::string              alias;

    // [Mount]
    std::string what;
    std::string where;
    std::string mount_type;
    std::string options;
    bool        lazy_unmount = false;

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> raw_sections;

    bool is_enabled() const {
        return !wanted_by.empty() || !required_by.empty();
    }

    /// True if this is a bare template unit (foo@.service — no instance)
    bool is_bare_template() const {
        auto at  = name.find('@');
        auto dot = name.rfind('.');
        return at != std::string::npos &&
               dot != std::string::npos &&
               at + 1 == dot;
    }
};

} // namespace tinit