#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/mixins/journal.hpp"
#include "trivialInit/systemd/unit_file.hpp"
#include "trivialInit/systemd/specifiers.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <charconv>

namespace tinit {

namespace detail {

inline std::string trim(std::string_view sv) {
    auto s = sv.find_first_not_of(" \t\r\n");
    if (s == std::string_view::npos) return {};
    auto e = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(s, e - s + 1));
}

inline std::vector<std::string> split_list(std::string_view sv) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < sv.size()) {
        // Skip spaces, tabs, and RS separators
        auto s = sv.find_first_not_of(" \t\x1f", pos);
        if (s == std::string_view::npos) break;
        auto e = sv.find_first_of(" \t\x1f", s);
        if (e == std::string_view::npos) e = sv.size();
        result.emplace_back(sv.substr(s, e - s));
        pos = e;
    }
    return result;
}

inline std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
parse_ini(const std::string& path) {
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;
    std::ifstream in(path);
    if (!in) return sections;

    std::string current_section, line, accumulated;

    auto process_line = [&](std::string_view raw) {
        auto trimmed = trim(raw);
        if (trimmed.empty() || trimmed[0]=='#' || trimmed[0]==';') return;
        if (trimmed.front()=='[' && trimmed.back()==']') {
            current_section = trimmed.substr(1, trimmed.size()-2);
            return;
        }
        auto eq = trimmed.find('=');
        if (eq == std::string::npos) return;
        auto key = trim(trimmed.substr(0, eq));
        auto val = trim(trimmed.substr(eq + 1));
        auto& existing = sections[current_section][key];
        // Condition*/Assert* can have multiple entries — store with index suffix
        // to avoid collapsing. We use a different approach: raw_sections stores
        // the last value per key; for conditions we scan raw_sections separately.
        existing = existing.empty() ? val : existing + "\x1f" + val; // RS separator
    };

    while (std::getline(in, line)) {
        if (!line.empty() && line.back()=='\\') {
            line.pop_back(); accumulated += line; continue;
        }
        if (!accumulated.empty()) {
            accumulated += line; process_line(accumulated); accumulated.clear();
        } else {
            process_line(line);
        }
    }
    if (!accumulated.empty()) process_line(accumulated);
    return sections;
}

/// Split a value string that may contain \x1f (RS) separators from repeated keys.
inline std::vector<std::string> split_rs(const std::string& s) {
    if (s.empty()) return {};
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos <= s.size()) {
        auto next = s.find('\x1f', pos);
        if (next == std::string::npos) next = s.size();
        auto tok = s.substr(pos, next - pos);
        if (!tok.empty()) result.push_back(std::move(tok));
        if (next == s.size()) break;
        pos = next + 1;
    }
    return result;
}

inline void parse_environment(std::string_view raw,
                               std::unordered_map<std::string, std::string>& out) {
    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && (raw[i]==' '||raw[i]=='\t')) ++i;
        if (i >= raw.size()) break;
        size_t eq = raw.find('=', i);
        if (eq == std::string_view::npos) break;
        std::string key(raw.substr(i, eq-i));
        i = eq + 1;
        std::string val;
        if (i < raw.size() && raw[i]=='"') {
            ++i;
            while (i < raw.size() && raw[i]!='"') {
                if (raw[i]=='\\' && i+1 < raw.size()) ++i;
                val += raw[i++];
            }
            if (i < raw.size()) ++i;
        } else {
            auto end = raw.find_first_of(" \t", i);
            if (end == std::string_view::npos) end = raw.size();
            val = std::string(raw.substr(i, end-i));
            i = end;
        }
        if (!key.empty()) out[key] = val;
    }
}

/// Parse a condition/assert entry from key=value.
/// value may start with '!' (negate) or '|' (or-group, simplified: treat as non-negated).
inline ConditionEntry parse_condition(const std::string& key, const std::string& val, bool is_assert) {
    ConditionEntry ce;
    ce.type      = key;
    ce.is_assert = is_assert;
    std::string_view v = val;
    // Strip leading | (or-mode — not fully implemented, treat as plain)
    if (!v.empty() && v[0] == '|') v = v.substr(1);
    if (!v.empty() && v[0] == '!') {
        ce.negate = true;
        v = v.substr(1);
    }
    ce.value = std::string(v);
    return ce;
}

/// All Condition* key names systemd supports
inline bool is_condition_key(std::string_view k) {
    static const char* conds[] = {
        "ConditionPathExists","ConditionPathExistsGlob","ConditionPathIsDirectory",
        "ConditionPathIsSymbolicLink","ConditionPathIsMountPoint","ConditionPathIsReadWrite",
        "ConditionPathIsEncrypted","ConditionDirectoryNotEmpty","ConditionFileNotEmpty",
        "ConditionFileIsExecutable","ConditionUser","ConditionGroup","ConditionHost",
        "ConditionKernelCommandLine","ConditionKernelVersion","ConditionEnvironment",
        "ConditionSecurity","ConditionCapability","ConditionACPower","ConditionNeedsUpdate",
        "ConditionFirstBoot","ConditionCPUs","ConditionMemory","ConditionArchitecture",
        "ConditionFirmware","ConditionVirtualization","ConditionOSRelease",
        "ConditionMemoryPressure","ConditionCPUPressure","ConditionIOPressure",
        nullptr
    };
    for (auto** p = conds; *p; ++p) if (k == *p) return true;
    return false;
}

inline bool is_assert_key(std::string_view k) {
    // Assert* mirrors Condition* 1:1
    return k.starts_with("Assert");
}

} // namespace detail

template <typename Derived>
struct UnitParseMixin : MixinBase<Derived, UnitParseMixin<Derived>> {
    std::unordered_map<std::string, UnitFile> units_;
    SpecifierResolver spec_resolver_;

    int execute(phase::UnitParse) {
        auto& s = this->self();
        s.log(LogLevel::Info, "parser", "Parsing unit files");

        for (const auto& path : s.unit_paths()) {
            auto sections = detail::parse_ini(path);
            UnitFile unit;
            unit.path = path;
            auto slash = path.rfind('/');
            unit.name = (slash != std::string::npos) ? path.substr(slash+1) : path;
            unit.type = unit_type_from_suffix(unit.name);
            unit.raw_sections = sections;

            SpecifierContext ctx(unit.name, unit.path);

            // [Unit]
            if (auto it = sections.find("Unit"); it != sections.end()) {
                auto& sec = it->second;
                if (sec.contains("Description")) unit.description = sec["Description"];
                if (sec.contains("Wants"))       unit.wants     = detail::split_list(sec["Wants"]);
                if (sec.contains("Requires"))    unit.requires_ = detail::split_list(sec["Requires"]);
                if (sec.contains("After"))       unit.after     = detail::split_list(sec["After"]);
                if (sec.contains("Before"))      unit.before    = detail::split_list(sec["Before"]);
                if (sec.contains("Conflicts"))   unit.conflicts = detail::split_list(sec["Conflicts"]);
                if (sec.contains("PartOf"))      unit.part_of   = detail::split_list(sec["PartOf"]);
                if (sec.contains("BindsTo"))     unit.binds_to  = detail::split_list(sec["BindsTo"]);

                // Conditions and Assertions
                for (auto& [key, val] : sec) {
                    if (detail::is_condition_key(key)) {
                        for (auto& v : detail::split_rs(val))
                            unit.conditions.push_back(detail::parse_condition(key, v, false));
                    } else if (detail::is_assert_key(key)) {
                        for (auto& v : detail::split_rs(val))
                            unit.assertions.push_back(detail::parse_condition(key, v, true));
                    }
                }
            }

            // [Service]
            if (auto it = sections.find("Service"); it != sections.end()) {
                auto& sec = it->second;
                if (sec.contains("ExecStart"))     unit.exec_start      = sec["ExecStart"];
                if (sec.contains("ExecStop"))      unit.exec_stop       = sec["ExecStop"];
                if (sec.contains("ExecReload"))    unit.exec_reload     = sec["ExecReload"];
                if (sec.contains("ExecStartPre"))  unit.exec_start_pre  = detail::split_rs(sec["ExecStartPre"]);
                if (sec.contains("ExecStartPost")) unit.exec_start_post = detail::split_rs(sec["ExecStartPost"]);
                if (sec.contains("Type"))          unit.type_str        = sec["Type"];
                if (sec.contains("Restart"))       unit.restart_policy  = sec["Restart"];
                if (sec.contains("User"))          unit.user            = sec["User"];
                if (sec.contains("Group"))         unit.group           = sec["Group"];
                if (sec.contains("WorkingDirectory"))
                    unit.working_directory = spec_resolver_.resolve(sec["WorkingDirectory"], ctx);
                if (sec.contains("RuntimeDirectory"))
                    unit.runtime_directory = spec_resolver_.resolve(sec["RuntimeDirectory"], ctx);
                if (sec.contains("PIDFile"))
                    unit.pid_file = spec_resolver_.resolve(sec["PIDFile"], ctx);
                if (sec.contains("StartLimitBurst")) {
                    int v=5; std::from_chars(sec["StartLimitBurst"].data(),
                        sec["StartLimitBurst"].data()+sec["StartLimitBurst"].size(),v);
                    unit.restart_limit_burst=v;
                }
                if (sec.contains("StartLimitIntervalSec")) {
                    int v=10; std::from_chars(sec["StartLimitIntervalSec"].data(),
                        sec["StartLimitIntervalSec"].data()+sec["StartLimitIntervalSec"].size(),v);
                    unit.restart_limit_interval=v;
                }
                if (sec.contains("Environment"))
                    detail::parse_environment(sec["Environment"], unit.environment);
                if (sec.contains("DynamicUser")) {
                    auto& v = sec["DynamicUser"];
                    unit.dynamic_user = (v=="yes"||v=="true"||v=="1");
                }
            }

            // [Install]
            if (auto it = sections.find("Install"); it != sections.end()) {
                auto& sec = it->second;
                if (sec.contains("WantedBy"))   unit.wanted_by  = detail::split_list(sec["WantedBy"]);
                if (sec.contains("RequiredBy")) unit.required_by= detail::split_list(sec["RequiredBy"]);
                if (sec.contains("Alias"))      unit.alias      = sec["Alias"];
            }

            // [Mount]
            if (auto it = sections.find("Mount"); it != sections.end()) {
                auto& sec = it->second;
                if (sec.contains("What"))    unit.what       = sec["What"];
                if (sec.contains("Where"))   unit.where      = sec["Where"];
                if (sec.contains("Type"))    unit.mount_type = sec["Type"];
                if (sec.contains("Options")) unit.options    = sec["Options"];
            }

            s.log_fmt(LogLevel::Debug, "parser", "Parsed: {} ({})", unit.name, unit.description);
            units_[unit.name] = std::move(unit);
        }

        s.log_fmt(LogLevel::Info, "parser", "Parsed {} units", units_.size());
        return 0;
    }

    const UnitFile* find_unit(const std::string& name) const {
        auto it = units_.find(name);
        return (it != units_.end()) ? &it->second : nullptr;
    }

    const auto& all_units() const { return units_; }
};

} // namespace tinit