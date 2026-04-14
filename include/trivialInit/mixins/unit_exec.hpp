#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/mixins/process.hpp"
#include "trivialInit/systemd/unit_file.hpp"
#include <sys/mount.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <algorithm>
#include <charconv>

namespace tinit {

namespace detail {

/// Split a mount Options= string into (MS_* flags, fs-data string).
/// Flags that map to mount(2) bits are consumed; remainder goes to data.
inline std::pair<unsigned long, std::string> parse_mount_options(const std::string& opts) {
    static const std::unordered_map<std::string, unsigned long> kFlagMap = {
        {"ro",           MS_RDONLY},
        {"rw",           0},                 // default; explicit no-op
        {"nosuid",       MS_NOSUID},
        {"suid",         0},
        {"nodev",        MS_NODEV},
        {"dev",          0},
        {"noexec",       MS_NOEXEC},
        {"exec",         0},
        {"sync",         MS_SYNCHRONOUS},
        {"async",        0},
        {"remount",      MS_REMOUNT},
        {"mand",         MS_MANDLOCK},
        {"nomand",       0},
        {"dirsync",      MS_DIRSYNC},
        {"noatime",      MS_NOATIME},
        {"atime",        0},
        {"nodiratime",   MS_NODIRATIME},
        {"diratime",     0},
        {"bind",         MS_BIND},
        {"rbind",        MS_BIND | MS_REC},
        {"move",         MS_MOVE},
        {"rec",          MS_REC},
        {"shared",       MS_SHARED},
        {"slave",        MS_SLAVE},
        {"private",      MS_PRIVATE},
        {"unbindable",   MS_UNBINDABLE},
        {"strictatime",  MS_STRICTATIME},
        {"lazytime",     MS_LAZYTIME},
        {"defaults",     0},                 // shorthand for rw,suid,dev,exec,auto,nouser,async
    };

    unsigned long flags = 0;
    std::string   data;

    // Options are comma-separated
    std::string_view sv = opts;
    while (!sv.empty()) {
        auto comma = sv.find(',');
        auto token = (comma == std::string_view::npos) ? sv : sv.substr(0, comma);
        sv = (comma == std::string_view::npos) ? std::string_view{} : sv.substr(comma + 1);

        std::string tok(token);
        auto it = kFlagMap.find(tok);
        if (it != kFlagMap.end()) {
            flags |= it->second;
        } else {
            if (!data.empty()) data += ',';
            data += tok;
        }
    }

    return {flags, data};
}

} // namespace detail (extends the one in unit_parse.hpp; same namespace, separate TU)

template <typename Derived>
struct UnitExecMixin : MixinBase<Derived, UnitExecMixin<Derived>> {
    std::vector<std::string> exec_order_;
    std::string default_target_ = "multi-user.target";

    // ----------------------------------------------------------------
    // Phase: DependencyResolve
    // ----------------------------------------------------------------
    int execute(phase::DependencyResolve) {
        auto& s = this->self();
        s.log(LogLevel::Info, "executor", "Resolving dependency graph");

        const auto& units = s.all_units();

        std::unordered_map<std::string, std::vector<std::string>> graph;
        std::unordered_map<std::string, int> in_degree;

        for (const auto& [name, _] : units) {
            graph[name];
            in_degree[name] = 0;
        }

        for (const auto& [name, unit] : units) {
            for (const auto& dep : unit.after) {
                if (units.contains(dep)) {
                    graph[dep].push_back(name);
                    in_degree[name]++;
                }
            }
            for (const auto& dep : unit.requires_) {
                if (units.contains(dep) &&
                    std::find(unit.after.begin(), unit.after.end(), dep) == unit.after.end()) {
                    graph[dep].push_back(name);
                    in_degree[name]++;
                }
            }
        }

        // Kahn's algorithm
        std::queue<std::string> ready;
        for (const auto& [name, deg] : in_degree)
            if (deg == 0) ready.push(name);

        exec_order_.clear();
        while (!ready.empty()) {
            auto current = ready.front();
            ready.pop();
            exec_order_.push_back(current);
            for (const auto& next : graph[current])
                if (--in_degree[next] == 0) ready.push(next);
        }

        if (exec_order_.size() < units.size()) {
            s.log_fmt(LogLevel::Warning, "executor",
                "Dependency cycle detected! Resolved {}/{} units",
                exec_order_.size(), units.size());
        }

        s.log_fmt(LogLevel::Info, "executor",
            "Execution order: {} units", exec_order_.size());
        return 0;
    }

    // ----------------------------------------------------------------
    // Phase: UnitExecute
    // ----------------------------------------------------------------
    int execute(phase::UnitExecute) {
        auto& s = this->self();
        s.log(LogLevel::Info, "executor", "Executing units in dependency order");

        for (const auto& name : exec_order_) {
            const auto* unit = s.find_unit(name);
            if (!unit) continue;

            switch (unit->type) {
            case UnitType::Target:
                s.log_fmt(LogLevel::Info, "executor", "Reached target: {}", name);
                break;

            case UnitType::Service:
                start_service(name, *unit);
                break;

            case UnitType::Mount:
                start_mount(name, *unit);
                break;

            case UnitType::Socket:
                // Handled by SocketMixin::execute(phase::SocketBind).
                s.log_fmt(LogLevel::Debug, "executor", "Socket unit ready: {}", name);
                break;

            default:
                s.log_fmt(LogLevel::Warning, "executor",
                    "Skipping unknown unit type: {}", name);
                break;
            }
        }

        return 0;
    }

    // ----------------------------------------------------------------
    // start_service — socket-activation aware
    // ----------------------------------------------------------------
    void start_service(const std::string& name, const UnitFile& unit) {
        auto& s = this->self();

        if (unit.exec_start.empty()) {
            s.log_fmt(LogLevel::Debug, "executor", "{}: no ExecStart, skipping", name);
            return;
        }

        std::string cmd = unit.exec_start;
        bool ignore_fail = false;
        if (!cmd.empty() && cmd[0] == '-') { ignore_fail = true; cmd = cmd.substr(1); }

        // Quoted argv split (handles simple quoting)
        auto args = split_exec(cmd);
        if (args.empty()) return;

        std::string exec_path = args[0];
        args.erase(args.begin());

        auto policy      = parse_restart_policy(unit.restart_policy);
        int  burst_limit = unit.restart_limit_burst;
        int  burst_ivl   = unit.restart_limit_interval;

        // Check for socket activation fds (SocketMixin adds collect_socket_env)
        if constexpr (requires { s.collect_socket_env(name); }) {
            auto env_opt = s.collect_socket_env(name);
            if (env_opt) {
                s.log_fmt(LogLevel::Info, "executor",
                    "Socket-activating {} ({} fd(s))", name, env_opt->fds.size());
                bool restart_flag = (policy != RestartPolicy::No);
                pid_t pid = s.spawn_with_socket_fds(name, exec_path, args, *env_opt, restart_flag);
                if (pid > 0) {
                    s.apply_restart_count(pid, name);
                    // Patch policy into the TrackedProcess
                    auto pit = s.processes_.find(pid);
                    if (pit != s.processes_.end()) {
                        pit->second.restart_policy   = policy;
                        pit->second.burst_limit      = burst_limit;
                        pit->second.burst_interval_s = burst_ivl;
                    }
                }
                return;
            }
        }

        s.log_fmt(LogLevel::Info, "executor", "Starting service: {} ({})", name, exec_path);
        pid_t pid = s.spawn(name, exec_path, args, false, policy, burst_limit, burst_ivl);
        if (pid > 0) {
            s.apply_restart_count(pid, name);
        } else if (!ignore_fail) {
            s.log_fmt(LogLevel::Error, "executor", "Failed to start {}", name);
        }
    }

    // ----------------------------------------------------------------
    // start_mount — proper MS_* flag splitting
    // ----------------------------------------------------------------
    void start_mount(const std::string& name, const UnitFile& unit) {
        auto& s = this->self();

        if (unit.what.empty() || unit.where.empty()) {
            s.log_fmt(LogLevel::Warning, "executor", "{}: incomplete mount unit", name);
            return;
        }

        s.log_fmt(LogLevel::Info, "executor",
            "Mounting {} on {}", unit.what, unit.where);

        // Ensure mountpoint exists
        ::mkdir(unit.where.c_str(), 0755);

        // Split Options= into mount(2) flags + fs-specific data string
        auto [flags, data] = detail::parse_mount_options(unit.options);

        int ret = ::mount(unit.what.c_str(),
                          unit.where.c_str(),
                          unit.mount_type.empty() ? nullptr : unit.mount_type.c_str(),
                          flags,
                          data.empty() ? nullptr : data.c_str());
        if (ret != 0) {
            s.log_fmt(LogLevel::Error, "executor",
                "mount {} failed: {}", name, strerror(errno));
        }
    }

    // ----------------------------------------------------------------
    // Restart machinery — driven by main loop, NOT by ProcessMixin.
    // ProcessMixin::reap_children() schedules into pending_restarts_;
    // process_restarts() fires them when their delay has elapsed.
    // ----------------------------------------------------------------
    void process_restarts() {
        auto& s = this->self();
        auto ready = s.drain_pending_restarts();
        for (const auto& name : ready) {
            const auto* unit = s.find_unit(name);
            if (unit && unit->type == UnitType::Service) {
                s.log_fmt(LogLevel::Notice, "executor", "Restarting {}", name);
                start_service(name, *unit);
            }
        }
    }

    // ----------------------------------------------------------------
    // Legacy queue_restart shim — kept for any code still calling it.
    // Routes through pending_restarts_ with zero delay.
    // ----------------------------------------------------------------
    void queue_restart(const std::string& unit_name) {
        // Zero-delay restart: schedule immediately (next main loop tick)
        // This shouldn't normally be called anymore — ProcessMixin now
        // directly populates pending_restarts_ with backoff — but keep
        // the shim to avoid breaking anything that calls it directly.
        auto& s = this->self();
        s.pending_restarts_.push_back({
            unit_name,
            std::chrono::steady_clock::now(),
            0
        });
    }

private:
    // ----------------------------------------------------------------
    // Quoted argv splitter: handles "foo bar" and 'foo bar' quoting.
    // ----------------------------------------------------------------
    static std::vector<std::string> split_exec(const std::string& s) {
        std::vector<std::string> result;
        std::string token;
        bool in_quote  = false;
        char quote_ch  = 0;

        for (char c : s) {
            if (in_quote) {
                if (c == quote_ch) { in_quote = false; }
                else               { token += c; }
            } else if (c == '"' || c == '\'') {
                in_quote = true; quote_ch = c;
            } else if (c == ' ' || c == '\t') {
                if (!token.empty()) { result.push_back(token); token.clear(); }
            } else {
                token += c;
            }
        }
        if (!token.empty()) result.push_back(token);
        return result;
    }
};

} // namespace tinit