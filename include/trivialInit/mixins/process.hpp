#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/systemd/unit_file.hpp"
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace tinit {

enum class ServiceState : uint8_t {
    Inactive, Starting, Running, Stopping, Stopped, Failed,
};

inline const char* state_str(ServiceState s) {
    switch (s) {
        case ServiceState::Inactive:  return "inactive";
        case ServiceState::Starting:  return "starting";
        case ServiceState::Running:   return "running";
        case ServiceState::Stopping:  return "stopping";
        case ServiceState::Stopped:   return "stopped";
        case ServiceState::Failed:    return "failed";
    }
    return "unknown";
}

enum class RestartPolicy : uint8_t {
    No, OnSuccess, OnFailure, OnAbnormal, OnWatchdog, OnAbort, Always,
};

inline RestartPolicy parse_restart_policy(std::string_view s) {
    if (s == "always")      return RestartPolicy::Always;
    if (s == "on-failure")  return RestartPolicy::OnFailure;
    if (s == "on-success")  return RestartPolicy::OnSuccess;
    if (s == "on-abnormal") return RestartPolicy::OnAbnormal;
    if (s == "on-watchdog") return RestartPolicy::OnWatchdog;
    if (s == "on-abort")    return RestartPolicy::OnAbort;
    return RestartPolicy::No;
}

inline bool should_restart(RestartPolicy policy, int exit_code, bool signaled) {
    bool clean = (!signaled && exit_code == 0);
    switch (policy) {
        case RestartPolicy::No:         return false;
        case RestartPolicy::Always:     return true;
        case RestartPolicy::OnSuccess:  return clean;
        case RestartPolicy::OnFailure:  return !clean;
        case RestartPolicy::OnAbnormal: return signaled || exit_code > 0;
        case RestartPolicy::OnAbort:    return signaled;
        case RestartPolicy::OnWatchdog: return false;
    }
    return false;
}

/// Resolve username → uid by parsing /etc/passwd directly (NSS-safe at PID 1)
inline uid_t lookup_uid(const std::string& name) {
    if (name.empty()) return (uid_t)-1;
    // Try numeric first
    bool all_digits = !name.empty();
    for (char c : name) if (!std::isdigit(c)) { all_digits = false; break; }
    if (all_digits) return (uid_t)std::stoul(name);

    std::ifstream f("/etc/passwd");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string field; int col = 0; std::string uname; uid_t uid = 0;
        while (std::getline(ss, field, ':')) {
            if (col == 0) uname = field;
            if (col == 2) { uid = (uid_t)std::stoul(field); break; }
            ++col;
        }
        if (uname == name) return uid;
    }
    return (uid_t)-1;
}

inline gid_t lookup_gid(const std::string& name) {
    if (name.empty()) return (gid_t)-1;
    bool all_digits = !name.empty();
    for (char c : name) if (!std::isdigit(c)) { all_digits = false; break; }
    if (all_digits) return (gid_t)std::stoul(name);

    std::ifstream f("/etc/group");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string field; int col = 0; std::string gname; gid_t gid = 0;
        while (std::getline(ss, field, ':')) {
            if (col == 0) gname = field;
            if (col == 2) { gid = (gid_t)std::stoul(field); break; }
            ++col;
        }
        if (gname == name) return gid;
    }
    return (gid_t)-1;
}

/// Get home directory for uid from /etc/passwd
inline std::string lookup_home(uid_t uid) {
    std::ifstream f("/etc/passwd");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string field; int col = 0; uid_t u = 0; std::string home;
        while (std::getline(ss, field, ':')) {
            if (col == 2) u = (uid_t)std::stoul(field);
            if (col == 5) { home = field; break; }
            ++col;
        }
        if (u == uid) return home;
    }
    return "/";
}

/// Setup child process credentials: drop privileges, chdir, inject env vars.
/// Called in child between fork() and exec().
/// Always returns true — unknown users silently run as root.
inline bool setup_child_context(const UnitFile& unit) {
    // --- RuntimeDirectory ---
    if (!unit.runtime_directory.empty()) {
        std::string rpath = "/run/" + unit.runtime_directory;
        ::mkdir(rpath.c_str(), 0755);
        if (!unit.user.empty()) {
            uid_t uid = lookup_uid(unit.user);
            gid_t gid = unit.group.empty() ? (gid_t)uid : lookup_gid(unit.group);
            if (uid != (uid_t)-1) ::chown(rpath.c_str(), uid, gid);
        }
    }

    // DynamicUser=yes: skip credential setup — run as root
    bool dynamic_user = unit.dynamic_user;
    if (!dynamic_user) {
        if (auto it = unit.raw_sections.find("Service"); it != unit.raw_sections.end()) {
            auto jt = it->second.find("DynamicUser");
            if (jt != it->second.end() &&
                (jt->second == "yes" || jt->second == "true" || jt->second == "1"))
                dynamic_user = true;
        }
    }

    uid_t target_uid = (uid_t)-1;
    gid_t target_gid = (gid_t)-1;

    if (!dynamic_user && !unit.user.empty()) {
        target_uid = lookup_uid(unit.user);
        if (target_uid != (uid_t)-1) {
            if (!unit.group.empty()) {
                target_gid = lookup_gid(unit.group);
                if (target_gid == (gid_t)-1) target_gid = target_uid;
            } else {
                target_gid = target_uid;
            }
            if (target_gid != (gid_t)-1) ::setgid(target_gid);
            ::initgroups(unit.user.c_str(), target_gid != (gid_t)-1 ? target_gid : 0);
            ::setuid(target_uid);
            ::setenv("USER",    unit.user.c_str(), 0);
            ::setenv("LOGNAME", unit.user.c_str(), 0);
            auto home = lookup_home(target_uid);
            ::setenv("HOME", home.c_str(), 0);
            if (!unit.working_directory.empty())
                ::chdir(unit.working_directory.c_str());
            else if (target_uid != 0)
                ::chdir(home.c_str());
            for (auto& [k, v] : unit.environment) ::setenv(k.c_str(), v.c_str(), 1);
            return true;
        }
        // Unknown user — fall through to root
    }

    if (!unit.working_directory.empty())
        ::chdir(unit.working_directory.c_str());
    for (auto& [k, v] : unit.environment) ::setenv(k.c_str(), v.c_str(), 1);
    return true;
}


struct TrackedProcess {
    pid_t           pid;
    std::string     unit_name;
    ServiceState    state;
    int             exit_code;
    RestartPolicy   restart_policy   = RestartPolicy::No;

    int             restart_count    = 0;
    std::chrono::steady_clock::time_point next_restart = {};

    int             burst_limit      = 5;
    int             burst_interval_s = 10;
    std::chrono::steady_clock::time_point burst_window_start = {};
    int             burst_count      = 0;

    static constexpr std::chrono::milliseconds kBaseDelay{100};
    static constexpr std::chrono::minutes      kMaxDelay{10};
};

template <typename Derived>
struct ProcessMixin : MixinBase<Derived, ProcessMixin<Derived>> {
    std::unordered_map<pid_t, TrackedProcess>           processes_;
    std::unordered_map<std::string, pid_t>              unit_to_pid_;
    std::unordered_map<pid_t, std::function<void(int)>> exit_callbacks_;

    struct PendingRestart {
        std::string                           unit_name;
        std::chrono::steady_clock::time_point when;
        int                                   restart_count;
        int                                   burst_count;
        std::chrono::steady_clock::time_point burst_window_start;
    };
    std::vector<PendingRestart> pending_restarts_;

    struct RestartMeta {
        int restart_count;
        int burst_count;
        std::chrono::steady_clock::time_point burst_window_start;
    };
    std::unordered_map<std::string, RestartMeta> restart_counts_;

    /// Spawn a service with full credential/environment setup.
    pid_t spawn(const std::string& unit_name,
                const std::string& exec_path,
                const std::vector<std::string>& args,
                bool /*compat*/ = false,
                RestartPolicy policy = RestartPolicy::No,
                int burst_limit = 5,
                int burst_interval_s = 10,
                const UnitFile* unit_ctx = nullptr)
    {
        auto& s = this->self();
        pid_t pid = ::fork();
        if (pid < 0) {
            s.log_fmt(LogLevel::Error, "process", "fork for {} failed: {}",
                unit_name, strerror(errno));
            return -1;
        }
        if (pid == 0) {
            sigset_t empty; sigemptyset(&empty);
            sigprocmask(SIG_SETMASK, &empty, nullptr);
            setsid();

            // Apply credentials, working directory, environment
            if (unit_ctx) {
                if (!setup_child_context(*unit_ctx)) _exit(126);
            }

            std::vector<const char*> argv;
            argv.push_back(exec_path.c_str());
            for (auto& a : args) argv.push_back(a.c_str());
            argv.push_back(nullptr);
            execvp(exec_path.c_str(), const_cast<char**>(argv.data()));
            _exit(127);
        }
        TrackedProcess tp{};
        tp.pid              = pid;
        tp.unit_name        = unit_name;
        tp.state            = ServiceState::Running;
        tp.exit_code        = -1;
        tp.restart_policy   = policy;
        tp.burst_limit      = burst_limit;
        tp.burst_interval_s = burst_interval_s;
        processes_[pid]         = std::move(tp);
        unit_to_pid_[unit_name] = pid;
        s.log_fmt(LogLevel::Info, "process", "Started {} (PID {})", unit_name, pid);
        return pid;
    }

    void track_process(pid_t pid, const std::string& unit_name, bool restart) {
        TrackedProcess tp{};
        tp.pid            = pid;
        tp.unit_name      = unit_name;
        tp.state          = ServiceState::Running;
        tp.exit_code      = -1;
        tp.restart_policy = restart ? RestartPolicy::OnFailure : RestartPolicy::No;
        processes_[pid]         = std::move(tp);
        unit_to_pid_[unit_name] = pid;
    }

    void on_child_exit(pid_t pid, std::function<void(int)> cb) {
        exit_callbacks_[pid] = std::move(cb);
    }

    void apply_restart_count(pid_t pid, const std::string& unit_name) {
        auto it = restart_counts_.find(unit_name);
        if (it == restart_counts_.end()) return;
        auto pit = processes_.find(pid);
        if (pit != processes_.end()) {
            pit->second.restart_count      = it->second.restart_count;
            pit->second.burst_count        = it->second.burst_count;
            pit->second.burst_window_start = it->second.burst_window_start;
        }
        restart_counts_.erase(it);
    }

    void reap_children() {
        auto& s = this->self();
        int status; pid_t pid;

        while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
            bool signaled = WIFSIGNALED(status);
            int  code     = signaled ? (128 + WTERMSIG(status)) : WEXITSTATUS(status);

            if (auto it = exit_callbacks_.find(pid); it != exit_callbacks_.end()) {
                it->second(code); exit_callbacks_.erase(it);
            }

            auto it = processes_.find(pid);
            if (it == processes_.end()) {
                s.log_fmt(LogLevel::Debug, "process", "Reaped orphan PID {} (exit {})", pid, code);
                continue;
            }

            auto tp = it->second;
            tp.state     = (code==0 && !signaled) ? ServiceState::Stopped : ServiceState::Failed;
            tp.exit_code = code;
            s.log_fmt(LogLevel::Info, "process", "{} (PID {}) exited with {}", tp.unit_name, pid, code);
            unit_to_pid_.erase(tp.unit_name);
            processes_.erase(it);

            if (!should_restart(tp.restart_policy, code, signaled)) continue;

            auto now = std::chrono::steady_clock::now();
            auto window = std::chrono::seconds(tp.burst_interval_s);
            if (now - tp.burst_window_start > window) {
                tp.burst_window_start = now; tp.burst_count = 0;
            }
            tp.burst_count++;

            if (tp.burst_count > tp.burst_limit) {
                s.log_fmt(LogLevel::Error, "process",
                    "{}: StartLimitBurst exhausted ({}/{}), not restarting",
                    tp.unit_name, tp.burst_count, tp.burst_limit);
                continue;
            }

            int exp = std::min(tp.restart_count, 13);
            auto delay = std::min(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    TrackedProcess::kBaseDelay * (1 << exp)),
                std::chrono::duration_cast<std::chrono::milliseconds>(TrackedProcess::kMaxDelay));

            s.log_fmt(LogLevel::Notice, "process",
                "{}: restart in {}ms (attempt {}, burst {}/{})",
                tp.unit_name, delay.count(), tp.restart_count+1, tp.burst_count, tp.burst_limit);

            pending_restarts_.push_back({tp.unit_name, now+delay,
                tp.restart_count+1, tp.burst_count, tp.burst_window_start});
        }
    }

    std::vector<std::string> drain_pending_restarts() {
        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> ready;
        std::vector<PendingRestart> remaining;
        for (auto& pr : pending_restarts_) {
            if (now >= pr.when) {
                restart_counts_[pr.unit_name] = {pr.restart_count, pr.burst_count, pr.burst_window_start};
                ready.push_back(pr.unit_name);
            } else {
                remaining.push_back(std::move(pr));
            }
        }
        pending_restarts_ = std::move(remaining);
        return ready;
    }

    int signal_unit(const std::string& unit_name, int sig) {
        auto it = unit_to_pid_.find(unit_name);
        if (it == unit_to_pid_.end()) return -1;
        return ::kill(it->second, sig);
    }

    int stop_unit(const std::string& unit_name) {
        auto& s = this->self();
        auto it = unit_to_pid_.find(unit_name);
        if (it == unit_to_pid_.end()) return 0;
        s.log_fmt(LogLevel::Info, "process", "Stopping {}", unit_name);
        auto pit = processes_.find(it->second);
        if (pit != processes_.end()) pit->second.restart_policy = RestartPolicy::No;
        ::kill(it->second, SIGTERM);
        return 0;
    }

    int active_count() const {
        int n = 0;
        for (auto& [_, tp] : processes_)
            if (tp.state == ServiceState::Running) ++n;
        return n;
    }
};

} // namespace tinit