#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include <string>
#include <vector>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace tinit {

struct MountPoint {
    const char* source;
    const char* target;
    const char* fstype;
    unsigned long flags;
    const char* data;
};

// Essential virtual filesystems that must be up before anything else
inline constexpr MountPoint kEarlyMounts[] = {
    {"proc",     "/proc",           "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr},
    {"sysfs",    "/sys",            "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr},
    {"devtmpfs", "/dev",            "devtmpfs", MS_NOSUID,                         "mode=0755"},
    {"tmpfs",    "/dev/shm",        "tmpfs",    MS_NOSUID | MS_NODEV,              nullptr},
    {"devpts",   "/dev/pts",        "devpts",   MS_NOSUID | MS_NOEXEC,             "gid=5,mode=620"},
    {"tmpfs",    "/run",            "tmpfs",    MS_NOSUID | MS_NODEV,              "mode=0755"},
    {"cgroup2",  "/sys/fs/cgroup",  "cgroup2",  MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr},
};

template <typename Derived>
struct FilesystemMixin : MixinBase<Derived, FilesystemMixin<Derived>> {
    std::vector<std::string> mounted_;

    int execute(phase::EarlyMount) {
        auto& s = this->self();
        s.log(LogLevel::Info, "fs", "Mounting early virtual filesystems");

        for (const auto& mp : kEarlyMounts) {
            // Ensure target exists
            ::mkdir(mp.target, 0755);

            int ret = ::mount(mp.source, mp.target, mp.fstype, mp.flags, mp.data);
            if (ret != 0 && errno != EBUSY) {
                s.log_fmt(LogLevel::Error, "fs", "mount {} on {} failed: {}",
                    mp.source, mp.target, ::strerror(errno));
                // Non-fatal for most; fatal for /proc
                if (std::string_view(mp.target) == "/proc") return 1;
                continue;
            }
            mounted_.emplace_back(mp.target);
            s.log_fmt(LogLevel::Info, "fs", "Mounted {} on {}", mp.fstype, mp.target);
        }

        // Create essential symlinks in /dev
        auto sl = [](const char* t, const char* l) { if (::symlink(t, l) && errno != EEXIST) {} };
        sl("/proc/self/fd",   "/dev/fd");
        sl("/proc/self/fd/0", "/dev/stdin");
        sl("/proc/self/fd/1", "/dev/stdout");
        sl("/proc/self/fd/2", "/dev/stderr");

        return 0;
    }

    int execute(phase::Shutdown) {
        auto& s = this->self();
        s.log(LogLevel::Info, "fs", "Unmounting filesystems");
        // Unmount in reverse order
        for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it) {
            if (::umount2(it->c_str(), MNT_DETACH) != 0) {
                s.log_fmt(LogLevel::Warning, "fs", "umount {} failed: {}",
                    *it, ::strerror(errno));
            }
        }
        return 0;
    }
};

} // namespace tinit
