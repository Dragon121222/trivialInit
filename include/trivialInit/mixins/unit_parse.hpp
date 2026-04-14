#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/mixins/journal.hpp"
#include "trivialInit/systemd/unit_file.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace tinit {

namespace detail {

inline std::string trim(std::string_view sv) {
    auto start = sv.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    auto end = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(start, end - start + 1));
}

inline std::vector<std::string> split_list(std::string_view sv) {
    std::vector<std::string> result;
    std::string_view delims = " \t";
    size_t pos = 0;
    while (pos < sv.size()) {
        auto start = sv.find_first_not_of(delims, pos);
        if (start == std::string_view::npos) break;
        auto end = sv.find_first_of(delims, start);
        if (end == std::string_view::npos) end = sv.size();
        result.emplace_back(sv.substr(start, end - start));
        pos = end;
    }
    return result;
}

/// Parse a single unit file from disk into raw section map
inline std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
parse_ini(const std::string& path) {
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;
    std::ifstream in(path);
    if (!in) return sections;

    std::string current_section;
    std::string line;

    while (std::getline(in, line)) {
        auto trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = trimmed.substr(1, trimmed.size() - 2);
            continue;
        }

        auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;

        auto key = trim(trimmed.substr(0, eq));
        auto val = trim(trimmed.substr(eq + 1));

        // Systemd allows repeated keys (e.g., multiple After= lines)
        // We concatenate with space for list-type keys
        auto& existing = sections[current_section][key];
        if (existing.empty()) {
            existing = val;
        } else {
            existing += " " + val;
        }
    }
    return sections;
}

} // namespace detail

template <typename Derived>
struct UnitParseMixin : MixinBase<Derived, UnitParseMixin<Derived>> {
    std::unordered_map<std::string, UnitFile> units_;

    int execute(phase::UnitParse) {
        auto& s = this->self();
        s.log(LogLevel::Info, "parser", "Parsing unit files");

        for (const auto& path : s.unit_paths()) {
            auto sections = detail::parse_ini(path);
            UnitFile unit;
            unit.path = path;

            // Extract filename as unit name
            auto slash = path.rfind('/');
            unit.name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
            unit.type = unit_type_from_suffix(unit.name);
            unit.raw_sections = sections;

            // [Unit]
            if (auto it = sections.find("Unit"); it != sections.end()) {
                auto& sec = it->second;
                if (sec.contains("Description")) unit.description = sec["Description"];
                if (sec.contains("Wants"))       unit.wants = detail::split_list(sec["Wants"]);
                if (sec.contains("Requires"))    unit.requires_ = detail::split_list(sec["Requires"]);
                if (sec.contains("After"))       unit.after = detail::split_list(sec["After"]);
                if (sec.contains("Before"))      unit.before = detail::split_list(sec["Before"]);
                if (sec.contains("Conflicts"))   unit.conflicts = detail::split_list(sec["Conflicts"]);
            }

            // [Service]
            if (auto it = sections.find("Service"); it != sections.end()) {
                auto& sec = it->second;
                if (sec.contains("ExecStart"))        unit.exec_start = sec["ExecStart"];
                if (sec.contains("ExecStop"))         unit.exec_stop = sec["ExecStop"];
                if (sec.contains("ExecStartPre"))     unit.exec_start_pre = detail::split_list(sec["ExecStartPre"]);
                if (sec.contains("Type"))             unit.type_str = sec["Type"];
                if (sec.contains("Restart"))          unit.restart_policy = sec["Restart"];
                if (sec.contains("User"))             unit.user = sec["User"];
                if (sec.contains("Group"))            unit.group = sec["Group"];
                if (sec.contains("WorkingDirectory")) unit.working_directory = sec["WorkingDirectory"];
            }

            // [Install]
            if (auto it = sections.find("Install"); it != sections.end()) {
                auto& sec = it->second;
                if (sec.contains("WantedBy"))    unit.wanted_by = detail::split_list(sec["WantedBy"]);
                if (sec.contains("RequiredBy"))  unit.required_by = detail::split_list(sec["RequiredBy"]);
                if (sec.contains("Alias"))       unit.alias = sec["Alias"];
            }

            // [Mount]
            if (auto it = sections.find("Mount"); it != sections.end()) {
                auto& sec = it->second;
                if (sec.contains("What"))    unit.what = sec["What"];
                if (sec.contains("Where"))   unit.where = sec["Where"];
                if (sec.contains("Type"))    unit.mount_type = sec["Type"];
                if (sec.contains("Options")) unit.options = sec["Options"];
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
