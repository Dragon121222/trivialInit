#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include <string>
#include <vector>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>

namespace tinit {

struct MountPoint {
    const char*   source;
    const char*   target;
    const char*   fstype;
    unsigned long flags;
    const char*   data;
};

inline constexpr MountPoint kEarlyMounts[] = {
    {"proc",     "/proc",          "proc",     MS_NOSUID|MS_NODEV|MS_NOEXEC, nullptr},
    {"sysfs",    "/sys",           "sysfs",    MS_NOSUID|MS_NODEV|MS_NOEXEC, nullptr},
    {"devtmpfs", "/dev",           "devtmpfs", MS_NOSUID,                     "mode=0755"},
    {"tmpfs",    "/dev/shm",       "tmpfs",    MS_NOSUID|MS_NODEV,            nullptr},
    {"devpts",   "/dev/pts",       "devpts",   MS_NOSUID|MS_NOEXEC,           "gid=5,mode=620"},
    {"tmpfs",    "/run",           "tmpfs",    MS_NOSUID|MS_NODEV,            "mode=0755"},
    {"cgroup2",  "/sys/fs/cgroup", "cgroup2",  MS_NOSUID|MS_NODEV|MS_NOEXEC, nullptr},
};

/// Runtime directories that need pre-creation.
/// chown is deferred until after systemd-sysusers runs.
struct RuntimeDir {
    const char* path;
    mode_t      mode;
    const char* owner;   // nullptr = root
    const char* group;   // nullptr = root
};

inline const RuntimeDir kRuntimeDirs[] = {
    {"/run/systemd",                              0755, nullptr,          nullptr},
    {"/run/systemd/resolve",                      0755, "systemd-resolve","systemd-resolve"},
    {"/run/systemd/netif",                        0755, "systemd-network","systemd-network"},
    {"/run/systemd/netif/links",                  0755, "systemd-network","systemd-network"},
    {"/run/systemd/netif/leases",                 0755, "systemd-network","systemd-network"},
    {"/run/systemd/netif/dhcp-server-lease",      0755, "systemd-network","systemd-network"},
    {"/run/systemd/journal",                      0755, nullptr,          nullptr},
    {"/run/systemd/ask-password",                 0755, nullptr,          nullptr},
    {"/run/dbus",                                 0755, nullptr,          nullptr},
    {"/run/udev",                                 0755, nullptr,          nullptr},
    {"/run/uuidd",                                0755, "uuidd",          "uuidd"},
    {"/run/systemd/report",                       0755, nullptr,          nullptr},
};

/// Parse /etc/passwd directly — avoids NSS/libc calls unsafe at early PID 1 init.
inline uid_t fs_resolve_uid(const char* name) {
    if (!name) return 0;
    std::ifstream f("/etc/passwd");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string field; int col=0; std::string uname; uid_t uid=0;
        while (std::getline(ss, field, ':')) {
            if (col==0) uname=field;
            if (col==2) { try { uid=(uid_t)std::stoul(field); } catch(...){} break; }
            ++col;
        }
        if (uname == name) return uid;
    }
    return 0; // fall back to root; chown(path, 0, 0) is a no-op
}

inline gid_t fs_resolve_gid(const char* name) {
    if (!name) return 0;
    std::ifstream f("/etc/group");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string field; int col=0; std::string gname; gid_t gid=0;
        while (std::getline(ss, field, ':')) {
            if (col==0) gname=field;
            if (col==2) { try { gid=(gid_t)std::stoul(field); } catch(...){} break; }
            ++col;
        }
        if (gname == name) return gid;
    }
    return 0;
}

template <typename Derived>
struct FilesystemMixin : MixinBase<Derived, FilesystemMixin<Derived>> {
    std::vector<std::string> mounted_;
    bool runtime_dirs_chowned_ = false;

    int execute(phase::EarlyMount) {
        auto& s = this->self();
        s.log(LogLevel::Info, "fs", "Mounting early virtual filesystems");

        for (const auto& mp : kEarlyMounts) {
            ::mkdir(mp.target, 0755);
            int ret = ::mount(mp.source, mp.target, mp.fstype, mp.flags, mp.data);
            if (ret != 0 && errno != EBUSY) {
                s.log_fmt(LogLevel::Error, "fs", "mount {} on {} failed: {}",
                    mp.source, mp.target, strerror(errno));
                if (std::string_view(mp.target) == "/proc") return 1;
                continue;
            }
            mounted_.emplace_back(mp.target);
            s.log_fmt(LogLevel::Info, "fs", "Mounted {} on {}", mp.fstype, mp.target);
        }

        // Essential /dev symlinks
        ::symlink("/proc/self/fd",   "/dev/fd");
        ::symlink("/proc/self/fd/0", "/dev/stdin");
        ::symlink("/proc/self/fd/1", "/dev/stdout");
        ::symlink("/proc/self/fd/2", "/dev/stderr");

        // Create runtime directories as root first.
        // Ownership will be fixed after systemd-sysusers runs.
        for (const auto& rd : kRuntimeDirs) {
            if (::mkdir(rd.path, rd.mode) != 0 && errno != EEXIST) continue;
            ::chmod(rd.path, rd.mode);
        }
        s.log(LogLevel::Info, "fs", "Pre-created runtime directories");

        return 0;
    }

    /// Called after systemd-sysusers exits successfully.
    /// Re-reads /etc/passwd and /etc/group (now fully populated) and chowns dirs.
    void fix_runtime_dir_ownership() {
        if (runtime_dirs_chowned_) return;
        runtime_dirs_chowned_ = true;

        for (const auto& rd : kRuntimeDirs) {
            if (!rd.owner && !rd.group) continue;
            uid_t uid = rd.owner ? fs_resolve_uid(rd.owner) : 0;
            gid_t gid = rd.group ? fs_resolve_gid(rd.group) : 0;
            // Only chown if we resolved to a non-root user (meaning the user now exists)
            if ((rd.owner && uid == 0) || (rd.group && gid == 0)) continue;
            ::chown(rd.path, uid, gid);
        }
    }

    int execute(phase::Shutdown) {
        auto& s = this->self();
        s.log(LogLevel::Info, "fs", "Unmounting filesystems");
        for (auto it = mounted_.rbegin(); it != mounted_.rend(); ++it) {
            if (::umount2(it->c_str(), MNT_DETACH) != 0)
                s.log_fmt(LogLevel::Warning, "fs", "umount {} failed: {}",
                    *it, strerror(errno));
        }
        return 0;
    }
};

} // namespace tinit