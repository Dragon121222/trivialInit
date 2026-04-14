#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace tinit {

enum class ServiceState : uint8_t {
    Inactive,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
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

struct TrackedProcess {
    pid_t pid;
    std::string unit_name;
    ServiceState state;
    int exit_code;
    bool restart;      // from unit file Restart= directive
};

template <typename Derived>
struct ProcessMixin : MixinBase<Derived, ProcessMixin<Derived>> {
    std::unordered_map<pid_t, TrackedProcess> processes_;
    std::unordered_map<std::string, pid_t> unit_to_pid_;

    /// Fork and exec a command, tracking it under a unit name
    pid_t spawn(const std::string& unit_name,
                const std::string& exec_path,
                const std::vector<std::string>& args,
                bool restart = false) {
        auto& s = this->self();

        pid_t pid = ::fork();
        if (pid < 0) {
            s.log_fmt(LogLevel::Error, "process", "fork for {} failed: {}",
                unit_name, strerror(errno));
            return -1;
        }

        if (pid == 0) {
            // Child: reset signals, exec
            sigset_t empty;
            sigemptyset(&empty);
            sigprocmask(SIG_SETMASK, &empty, nullptr);

            // Create new session
            setsid();

            // Build argv
            std::vector<const char*> argv;
            argv.push_back(exec_path.c_str());
            for (auto& a : args) argv.push_back(a.c_str());
            argv.push_back(nullptr);

            execvp(exec_path.c_str(), const_cast<char**>(argv.data()));
            // If we get here, exec failed
            _exit(127);
        }

        // Parent: track
        TrackedProcess tp{pid, unit_name, ServiceState::Running, -1, restart};
        processes_[pid] = tp;
        unit_to_pid_[unit_name] = pid;
        s.log_fmt(LogLevel::Info, "process", "Started {} (PID {})", unit_name, pid);
        return pid;
    }

    /// Reap all finished children — called from signal handler
    void reap_children() {
        auto& s = this->self();
        int status;
        pid_t pid;

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            auto it = processes_.find(pid);
            if (it != processes_.end()) {
                auto& tp = it->second;
                tp.exit_code = code;
                tp.state = (code == 0) ? ServiceState::Stopped : ServiceState::Failed;
                s.log_fmt(LogLevel::Info, "process", "{} (PID {}) exited with {}",
                    tp.unit_name, pid, code);

                unit_to_pid_.erase(tp.unit_name);

                // Handle restart policy
                if (tp.restart && tp.state == ServiceState::Failed) {
                    s.log_fmt(LogLevel::Notice, "process", "Restarting {}", tp.unit_name);
                    // Queue restart — actual re-exec handled by executor
                    s.queue_restart(tp.unit_name);
                }

                processes_.erase(it);
            } else {
                // Unknown child (adopted zombie)
                s.log_fmt(LogLevel::Debug, "process", "Reaped orphan PID {} (exit {})", pid, code);
            }
        }
    }

    /// Send signal to a tracked service
    int signal_unit(const std::string& unit_name, int sig) {
        auto it = unit_to_pid_.find(unit_name);
        if (it == unit_to_pid_.end()) return -1;
        return ::kill(it->second, sig);
    }

    /// Stop a service
    int stop_unit(const std::string& unit_name) {
        auto& s = this->self();
        auto it = unit_to_pid_.find(unit_name);
        if (it == unit_to_pid_.end()) return 0; // already dead
        s.log_fmt(LogLevel::Info, "process", "Stopping {}", unit_name);

        // Disable restart
        auto pit = processes_.find(it->second);
        if (pit != processes_.end()) pit->second.restart = false;

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
