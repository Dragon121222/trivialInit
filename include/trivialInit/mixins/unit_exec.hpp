#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/systemd/unit_file.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <algorithm>

namespace tinit {

template <typename Derived>
struct UnitExecMixin : MixinBase<Derived, UnitExecMixin<Derived>> {
    std::vector<std::string> exec_order_;       // topological order
    std::vector<std::string> restart_queue_;
    std::string default_target_ = "multi-user.target";

    int execute(phase::DependencyResolve) {
        auto& s = this->self();
        s.log(LogLevel::Info, "executor", "Resolving dependency graph");

        // Build adjacency list from After= relationships
        // If A has After=B, then B must start before A → edge B→A
        const auto& units = s.all_units();

        std::unordered_map<std::string, std::vector<std::string>> graph;
        std::unordered_map<std::string, int> in_degree;

        // Initialize all known units
        for (const auto& [name, _] : units) {
            graph[name];
            in_degree[name] = 0;
        }

        // Build edges from After= (and implicitly from Requires=)
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
                    // Requires implies After if not already stated
                    graph[dep].push_back(name);
                    in_degree[name]++;
                }
            }
        }

        // Kahn's algorithm for topological sort
        std::queue<std::string> ready;
        for (const auto& [name, deg] : in_degree) {
            if (deg == 0) ready.push(name);
        }

        exec_order_.clear();
        while (!ready.empty()) {
            auto current = ready.front();
            ready.pop();
            exec_order_.push_back(current);

            for (const auto& next : graph[current]) {
                if (--in_degree[next] == 0) {
                    ready.push(next);
                }
            }
        }

        // Cycle detection
        if (exec_order_.size() < units.size()) {
            s.log_fmt(LogLevel::Warning, "executor",
                "Dependency cycle detected! Resolved {}/{} units",
                exec_order_.size(), units.size());
        }

        s.log_fmt(LogLevel::Info, "executor", "Execution order: {} units", exec_order_.size());
        return 0;
    }

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

            default:
                s.log_fmt(LogLevel::Warning, "executor", "Skipping unknown unit type: {}", name);
                break;
            }
        }

        return 0;
    }

    void start_service(const std::string& name, const UnitFile& unit) {
        auto& s = this->self();

        if (unit.exec_start.empty()) {
            s.log_fmt(LogLevel::Debug, "executor", "{}: no ExecStart, skipping", name);
            return;
        }

        // Parse ExecStart into command + args
        // Handle optional prefixes: -, @, +, !, !!
        std::string cmd = unit.exec_start;
        bool ignore_fail = false;
        if (!cmd.empty() && cmd[0] == '-') {
            ignore_fail = true;
            cmd = cmd.substr(1);
        }

        auto args = detail::split_list(cmd);
        if (args.empty()) return;

        std::string exec_path = args[0];
        args.erase(args.begin());

        bool restart = (unit.restart_policy == "always" || unit.restart_policy == "on-failure");

        s.log_fmt(LogLevel::Info, "executor", "Starting service: {} ({})", name, exec_path);
        pid_t pid = s.spawn(name, exec_path, args, restart);

        if (pid < 0 && !ignore_fail) {
            s.log_fmt(LogLevel::Error, "executor", "Failed to start {}", name);
        }
    }

    void start_mount(const std::string& name, const UnitFile& unit) {
        auto& s = this->self();

        if (unit.what.empty() || unit.where.empty()) {
            s.log_fmt(LogLevel::Warning, "executor", "{}: incomplete mount unit", name);
            return;
        }

        s.log_fmt(LogLevel::Info, "executor", "Mounting {} on {}", unit.what, unit.where);

        // Use mount(2) directly
        ::mkdir(unit.where.c_str(), 0755);
        int ret = ::mount(unit.what.c_str(), unit.where.c_str(),
                          unit.mount_type.c_str(), 0,
                          unit.options.empty() ? nullptr : unit.options.c_str());
        if (ret != 0) {
            s.log_fmt(LogLevel::Error, "executor", "mount {} failed: {}",
                name, strerror(errno));
        }
    }

    void queue_restart(const std::string& unit_name) {
        restart_queue_.push_back(unit_name);
    }

    /// Process pending restarts
    void process_restarts() {
        auto& s = this->self();
        auto queue = std::move(restart_queue_);
        restart_queue_.clear();

        for (const auto& name : queue) {
            const auto* unit = s.find_unit(name);
            if (unit && unit->type == UnitType::Service) {
                s.log_fmt(LogLevel::Notice, "executor", "Restarting {}", name);
                start_service(name, *unit);
            }
        }
    }
};

} // namespace tinit
