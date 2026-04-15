#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/systemd/unit_file.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <set>

namespace tinit {

inline const std::vector<std::string> kUnitSearchPaths = {
    "/etc/systemd/system",
    "/run/systemd/system",
    "/usr/lib/systemd/system",
    "/lib/systemd/system",
};

inline const std::vector<std::string> kSupportedExtensions = {
    ".service", ".target", ".mount", ".socket",
};

template <typename Derived>
struct UnitScanMixin : MixinBase<Derived, UnitScanMixin<Derived>> {
    std::vector<std::string> discovered_paths_;
    std::set<std::string>    discovered_names_;

    int execute(phase::UnitDiscovery) {
        auto& s = this->self();
        s.log(LogLevel::Info, "scanner", "Scanning for unit files");

        namespace fs = std::filesystem;
        std::error_code ec;

        // Pass 1: direct unit files in each search directory.
        // Higher-priority paths first — /etc overrides /usr/lib for same filename.
        for (const auto& search_dir : kUnitSearchPaths) {
            if (!fs::exists(search_dir)) continue;

            for (const auto& entry : fs::directory_iterator(search_dir, ec)) {
                if (!entry.is_regular_file() && !entry.is_symlink()) continue;
                auto filename = entry.path().filename().string();
                if (!is_supported(filename)) continue;
                if (discovered_names_.contains(filename)) continue;
                discovered_names_.insert(filename);
                discovered_paths_.push_back(entry.path().string());
                s.log_fmt(LogLevel::Debug, "scanner", "Found: {}", entry.path().string());
            }
        }

        // Pass 2: *.wants/ and *.requires/ directories.
        //
        // systemctl enable creates symlinks like:
        //   /etc/systemd/system/getty.target.wants/getty@tty1.service
        //     -> /usr/lib/systemd/system/getty@.service
        //
        // These pull in specific enabled instances that wouldn't otherwise be
        // discovered.  We resolve each symlink and add the target if not already
        // known.  Relative symlinks are resolved relative to the .wants/ dir.
        for (const auto& search_dir : kUnitSearchPaths) {
            if (!fs::exists(search_dir)) continue;

            for (const auto& entry : fs::directory_iterator(search_dir, ec)) {
                if (!entry.is_directory()) continue;
                auto dname = entry.path().filename().string();
                if (!dname.ends_with(".wants") && !dname.ends_with(".requires"))
                    continue;

                for (const auto& link : fs::directory_iterator(entry.path(), ec)) {
                    fs::path target;
                    if (link.is_symlink()) {
                        target = fs::read_symlink(link.path(), ec);
                        if (ec) { ec.clear(); continue; }
                        if (target.is_relative())
                            target = entry.path() / target;
                        target = fs::weakly_canonical(target, ec);
                        if (ec) { ec.clear(); continue; }
                    } else {
                        target = fs::weakly_canonical(link.path(), ec);
                        if (ec) { ec.clear(); continue; }
                    }

                    if (!fs::exists(target, ec)) { ec.clear(); continue; }

                    auto fname = target.filename().string();
                    if (!is_supported(fname)) continue;

                    // The symlink name in .wants/ IS the instance name
                    // (e.g. getty@tty1.service), not the template target name
                    // (getty@.service).  Use the link's own filename as the
                    // unit name so the instance is addressable by its full name.
                    auto link_fname = link.path().filename().string();
                    if (!is_supported(link_fname)) link_fname = fname;

                    if (discovered_names_.contains(link_fname)) continue;

                    // The actual file on disk is the template (e.g. getty@.service).
                    // We register it under the instance name so the executor can
                    // resolve specifiers (%i, %I, etc.) correctly at exec time.
                    discovered_names_.insert(link_fname);
                    // Store as pair: path=template file, but name=instance name.
                    // We achieve this by storing the resolved template path but
                    // recording the instance name in discovered_names_.
                    // UnitParseMixin will use the path for content but set
                    // unit.name from the last path component — so we need to
                    // store under a synthetic path whose basename IS the instance.
                    // Easiest: create a symlink-like entry by storing the
                    // template path; the parser will name it from the path.
                    // Instead store the template path and rely on the fact that
                    // the .wants/ link name IS what we want as the unit name.
                    // We do this by inserting a virtual mapping: push the
                    // template path but associate it with the link name.
                    // Since UnitParseMixin names units by path basename, we
                    // push the *link path* (which has the instance name) if
                    // it is a symlink that the filesystem can dereference at
                    // parse time — which it can, since fs::weakly_canonical
                    // resolved it. Use the original link path as the path to
                    // store so the parser sees "getty@tty1.service" as the name.
                    discovered_paths_.push_back(link.path().string());
                    s.log_fmt(LogLevel::Debug, "scanner",
                        "Found (wants): {} -> {}", link_fname, target.string());
                }
            }
        }

        s.log_fmt(LogLevel::Info, "scanner",
            "Discovered {} unit files", discovered_paths_.size());
        return 0;
    }

    const std::vector<std::string>& unit_paths() const { return discovered_paths_; }
    bool has_unit(const std::string& name) const { return discovered_names_.contains(name); }

private:
    static bool is_supported(const std::string& filename) {
        for (const auto& ext : kSupportedExtensions)
            if (filename.ends_with(ext)) return true;
        return false;
    }
};

} // namespace tinit