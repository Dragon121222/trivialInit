#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/systemd/unit_file.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <set>

namespace tinit {

/// Systemd-compatible unit file search paths (in priority order)
inline const std::vector<std::string> kUnitSearchPaths = {
    "/etc/systemd/system",          // admin overrides (highest priority)
    "/run/systemd/system",          // runtime units
    "/usr/lib/systemd/system",      // package-installed units
    "/lib/systemd/system",          // legacy path
};

inline const std::vector<std::string> kSupportedExtensions = {
    ".service", ".target", ".mount",
};

template <typename Derived>
struct UnitScanMixin : MixinBase<Derived, UnitScanMixin<Derived>> {
    std::vector<std::string> discovered_paths_;
    std::set<std::string> discovered_names_;  // dedup by name (highest priority wins)

    int execute(phase::UnitDiscovery) {
        auto& s = this->self();
        s.log(LogLevel::Info, "scanner", "Scanning for unit files");

        namespace fs = std::filesystem;

        for (const auto& search_dir : kUnitSearchPaths) {
            if (!fs::exists(search_dir)) continue;

            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(search_dir, ec)) {
                if (!entry.is_regular_file() && !entry.is_symlink()) continue;

                auto filename = entry.path().filename().string();

                // Check extension
                bool supported = false;
                for (const auto& ext : kSupportedExtensions) {
                    if (filename.ends_with(ext)) { supported = true; break; }
                }
                if (!supported) continue;

                // Priority: first path wins (highest priority dirs are first)
                if (discovered_names_.contains(filename)) continue;

                discovered_names_.insert(filename);
                discovered_paths_.push_back(entry.path().string());
                s.log_fmt(LogLevel::Debug, "scanner", "Found: {}", entry.path().string());
            }
        }

        s.log_fmt(LogLevel::Info, "scanner", "Discovered {} unit files", discovered_paths_.size());
        return 0;
    }

    /// Get list of discovered unit file paths
    const std::vector<std::string>& unit_paths() const { return discovered_paths_; }

    /// Check if a specific unit was found
    bool has_unit(const std::string& name) const {
        return discovered_names_.contains(name);
    }
};

} // namespace tinit
