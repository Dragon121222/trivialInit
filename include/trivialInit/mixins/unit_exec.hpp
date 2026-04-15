#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/mixins/process.hpp"
#include "trivialInit/systemd/unit_file.hpp"
#include "trivialInit/systemd/specifiers.hpp"
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <algorithm>
#include <charconv>
#include <fstream>

namespace tinit {

namespace detail {

inline std::pair<unsigned long, std::string> parse_mount_options(const std::string& opts) {
    static const std::unordered_map<std::string, unsigned long> kFlagMap = {
        {"ro",MS_RDONLY},{"rw",0},{"nosuid",MS_NOSUID},{"suid",0},
        {"nodev",MS_NODEV},{"dev",0},{"noexec",MS_NOEXEC},{"exec",0},
        {"sync",MS_SYNCHRONOUS},{"async",0},{"remount",MS_REMOUNT},
        {"mand",MS_MANDLOCK},{"nomand",0},{"dirsync",MS_DIRSYNC},
        {"noatime",MS_NOATIME},{"atime",0},{"nodiratime",MS_NODIRATIME},
        {"diratime",0},{"bind",MS_BIND},{"rbind",MS_BIND|MS_REC},
        {"move",MS_MOVE},{"rec",MS_REC},{"shared",MS_SHARED},
        {"slave",MS_SLAVE},{"private",MS_PRIVATE},{"unbindable",MS_UNBINDABLE},
        {"strictatime",MS_STRICTATIME},{"lazytime",MS_LAZYTIME},
        // tmpfs-specific flags that kernel rejects as data options — map to MS_* or discard
        {"noswap",0},          // not a real flag — discard
        {"defaults",0},
    };
    unsigned long flags = 0;
    std::string data;
    std::string_view sv = opts;
    while (!sv.empty()) {
        auto comma = sv.find(',');
        auto token = (comma==std::string_view::npos) ? sv : sv.substr(0,comma);
        sv = (comma==std::string_view::npos) ? std::string_view{} : sv.substr(comma+1);
        // Handle key=value tokens (e.g. mode=1777, uid=0) — these are fs data, not flags
        if (token.find('=') != std::string_view::npos) {
            if (!data.empty()) data += ',';
            data += std::string(token);
            continue;
        }
        std::string tok(token);
        auto it = kFlagMap.find(tok);
        if (it != kFlagMap.end()) {
            flags |= it->second;
        } else if (!tok.empty()) {
            // Unknown token — pass as data (may be fs-specific like size=, nr_inodes=)
            if (!data.empty()) data += ',';
            data += tok;
        }
    }
    return {flags, data};
}

} // namespace detail

// ----------------------------------------------------------------
// Condition evaluator
// ----------------------------------------------------------------
namespace cond {

inline bool eval_one(const ConditionEntry& ce) {
    bool result = false;
    const std::string& val = ce.value;

    if (ce.type == "ConditionPathExists" || ce.type == "AssertPathExists") {
        result = (::access(val.c_str(), F_OK) == 0);
    } else if (ce.type == "ConditionPathExistsGlob" || ce.type == "AssertPathExistsGlob") {
        // Simple glob: just check if a matching file exists (use fnmatch on parent dir)
        auto slash = val.rfind('/');
        std::string dir  = (slash == std::string::npos) ? "." : val.substr(0, slash);
        std::string pat  = (slash == std::string::npos) ? val : val.substr(slash+1);
        bool found = false;
        if (auto* d = opendir(dir.c_str())) {
            struct dirent* e;
            while ((e = readdir(d))) {
                if (fnmatch(pat.c_str(), e->d_name, 0) == 0) { found = true; break; }
            }
            closedir(d);
        }
        result = found;
    } else if (ce.type == "ConditionPathIsDirectory" || ce.type == "AssertPathIsDirectory") {
        struct stat st{}; result = (::stat(val.c_str(), &st)==0 && S_ISDIR(st.st_mode));
    } else if (ce.type == "ConditionPathIsSymbolicLink" || ce.type == "AssertPathIsSymbolicLink") {
        struct stat st{}; result = (::lstat(val.c_str(), &st)==0 && S_ISLNK(st.st_mode));
    } else if (ce.type == "ConditionPathIsMountPoint" || ce.type == "AssertPathIsMountPoint") {
        // Check if path and parent have different device numbers
        struct stat st, pst{};
        std::string parent = val + "/..";
        result = (::stat(val.c_str(), &st)==0 && ::stat(parent.c_str(), &pst)==0
                  && st.st_dev != pst.st_dev);
    } else if (ce.type == "ConditionPathIsReadWrite" || ce.type == "AssertPathIsReadWrite") {
        result = (::access(val.c_str(), W_OK) == 0);
    } else if (ce.type == "ConditionFileNotEmpty" || ce.type == "AssertFileNotEmpty") {
        struct stat st{}; result = (::stat(val.c_str(), &st)==0 && st.st_size > 0);
    } else if (ce.type == "ConditionDirectoryNotEmpty" || ce.type == "AssertDirectoryNotEmpty") {
        if (auto* d = opendir(val.c_str())) {
            struct dirent* e; int count = 0;
            while ((e = readdir(d))) {
                if (std::string_view(e->d_name) != "." && std::string_view(e->d_name) != "..") {
                    ++count; break;
                }
            }
            closedir(d);
            result = (count > 0);
        }
    } else if (ce.type == "ConditionFileIsExecutable" || ce.type == "AssertFileIsExecutable") {
        result = (::access(val.c_str(), X_OK) == 0);
    } else if (ce.type == "ConditionKernelCommandLine" || ce.type == "AssertKernelCommandLine") {
        std::ifstream f("/proc/cmdline");
        std::string cmdline; std::getline(f, cmdline);
        // Match whole word or key=value presence
        result = (cmdline.find(val) != std::string::npos);
    } else if (ce.type == "ConditionKernelVersion" || ce.type == "AssertKernelVersion") {
        struct utsname uts; uname(&uts);
        // Simple version string containment check
        result = (std::string_view(uts.release).find(val) != std::string_view::npos);
    } else if (ce.type == "ConditionVirtualization" || ce.type == "AssertVirtualization") {
        // Check for QEMU/KVM via /proc/cpuinfo or /sys
        if (val == "no") {
            // True if NOT virtualized — we're in QEMU so this is false
            result = false;
        } else if (val == "vm" || val == "kvm" || val == "qemu") {
            std::ifstream f("/proc/cpuinfo");
            std::string line;
            while (std::getline(f, line))
                if (line.find("QEMU") != std::string::npos ||
                    line.find("KVM")  != std::string::npos) { result = true; break; }
        } else {
            result = true; // Unknown virt type — assume present
        }
    } else if (ce.type == "ConditionACPower" || ce.type == "AssertACPower") {
        // In QEMU we have no battery; treat as on AC
        result = (val == "true" || val == "1");
    } else if (ce.type == "ConditionUser" || ce.type == "AssertUser") {
        uid_t uid = getuid();
        if (val == "root") result = (uid == 0);
        else if (val == "@system") result = (uid < 1000);
        else {
            uid_t target = lookup_uid(val);
            result = (target != (uid_t)-1 && uid == target);
        }
    } else if (ce.type == "ConditionFirstBoot" || ce.type == "AssertFirstBoot") {
        // Check /run/systemd/first-boot or /etc/.updated
        bool first = (::access("/run/systemd/first-boot", F_OK) == 0);
        result = (val == "yes" || val == "true" || val == "1") ? first : !first;
    } else if (ce.type == "ConditionEnvironment" || ce.type == "AssertEnvironment") {
        auto eq = val.find('=');
        if (eq == std::string::npos) {
            result = (::getenv(val.c_str()) != nullptr);
        } else {
            auto* v = ::getenv(val.substr(0, eq).c_str());
            result = (v && std::string(v) == val.substr(eq+1));
        }
    } else if (ce.type == "ConditionSecurity" || ce.type == "AssertSecurity") {
        // SELinux/AppArmor/etc. — just check if the named module is active
        result = false; // conservative: assume not active in minimal QEMU
    } else if (ce.type == "ConditionCapability" || ce.type == "AssertCapability") {
        result = (getuid() == 0); // root has all capabilities
    } else {
        // Unknown condition — pass through (don't block unit)
        result = true;
    }

    return ce.negate ? !result : result;
}

/// Evaluate all conditions for a unit.
/// Returns: 0 = pass, 1 = skip (condition false), 2 = fail (assertion false)
inline int eval_conditions(const UnitFile& unit) {
    // Conditions: ALL must be true (or or-group semantics ignored for now)
    for (auto& ce : unit.conditions) {
        if (!eval_one(ce)) return 1; // skip silently
    }
    // Assertions: ALL must be true
    for (auto& ce : unit.assertions) {
        if (!eval_one(ce)) return 2; // fail with error
    }
    return 0;
}

} // namespace cond

template <typename Derived>
struct UnitExecMixin : MixinBase<Derived, UnitExecMixin<Derived>> {
    std::vector<std::string> exec_order_;
    std::string default_target_ = "multi-user.target";
    SpecifierResolver spec_resolver_;

    // ----------------------------------------------------------------
    int execute(phase::DependencyResolve) {
        auto& s = this->self();
        s.log(LogLevel::Info, "executor", "Resolving dependency graph");

        const auto& units = s.all_units();
        std::unordered_map<std::string, std::vector<std::string>> graph;
        std::unordered_map<std::string, int> in_degree;

        for (const auto& [name, _] : units) { graph[name]; in_degree[name]=0; }

        for (const auto& [name, unit] : units) {
            for (const auto& dep : unit.after) {
                if (units.contains(dep)) { graph[dep].push_back(name); in_degree[name]++; }
            }
            for (const auto& dep : unit.requires_) {
                if (units.contains(dep) &&
                    std::find(unit.after.begin(),unit.after.end(),dep)==unit.after.end()) {
                    graph[dep].push_back(name); in_degree[name]++;
                }
            }
        }

        std::queue<std::string> ready;
        for (const auto& [name,deg] : in_degree) if (deg==0) ready.push(name);
        exec_order_.clear();
        while (!ready.empty()) {
            auto cur = ready.front(); ready.pop();
            exec_order_.push_back(cur);
            for (const auto& next : graph[cur])
                if (--in_degree[next]==0) ready.push(next);
        }

        if (exec_order_.size() < units.size())
            s.log_fmt(LogLevel::Warning,"executor",
                "Dependency cycle detected! Resolved {}/{} units",
                exec_order_.size(), units.size());

        s.log_fmt(LogLevel::Info,"executor","Execution order: {} units",exec_order_.size());
        return 0;
    }

    // ----------------------------------------------------------------
    int execute(phase::UnitExecute) {
        auto& s = this->self();
        s.log(LogLevel::Info, "executor", "Executing units in dependency order");

        for (const auto& name : exec_order_) {
            const auto* unit = s.find_unit(name);
            if (!unit) continue;

            // Skip bare template units — they have no instance
            if (unit->is_bare_template()) {
                s.log_fmt(LogLevel::Debug,"executor","Skipping bare template: {}",name);
                continue;
            }

            // Evaluate conditions
            int cond_result = cond::eval_conditions(*unit);
            if (cond_result == 1) {
                s.log_fmt(LogLevel::Debug,"executor","{}: condition not met, skipping",name);
                continue;
            }
            if (cond_result == 2) {
                s.log_fmt(LogLevel::Error,"executor","{}: assertion failed",name);
                continue;
            }

            switch (unit->type) {
            case UnitType::Target:
                s.log_fmt(LogLevel::Info,"executor","Reached target: {}",name); break;
            case UnitType::Service:
                start_service(name, *unit); break;
            case UnitType::Mount:
                start_mount(name, *unit); break;
            case UnitType::Socket:
                s.log_fmt(LogLevel::Debug,"executor","Socket unit ready: {}",name); break;
            default:
                s.log_fmt(LogLevel::Warning,"executor","Skipping unknown unit type: {}",name); break;
            }
        }
        return 0;
    }

    // ----------------------------------------------------------------
    void start_service(const std::string& name, const UnitFile& unit) {
        auto& s = this->self();

        // Create RuntimeDirectory before starting
        if (!unit.runtime_directory.empty()) {
            std::string rpath = "/run/" + unit.runtime_directory;
            ::mkdir(rpath.c_str(), 0755);
            if (!unit.user.empty()) {
                uid_t uid = lookup_uid(unit.user);
                gid_t gid = unit.group.empty() ? (gid_t)uid : lookup_gid(unit.group);
                if (uid != (uid_t)-1) ::chown(rpath.c_str(), uid, gid);
            }
        }

        // Run ExecStartPre commands synchronously
        if (!unit.exec_start_pre.empty()) {
            SpecifierContext ctx(name, unit.path);
            for (auto& pre_cmd : unit.exec_start_pre) {
                std::string cmd = pre_cmd;
                bool ignore = false;
                if (!cmd.empty() && cmd[0]=='-') { ignore=true; cmd=cmd.substr(1); }
                cmd = spec_resolver_.resolve(cmd, ctx);
                auto args = split_exec(cmd);
                if (args.empty()) continue;
                std::string ep = args[0]; args.erase(args.begin());
                for (auto& a : args) a = spec_resolver_.resolve(a, ctx);

                pid_t pid = ::fork();
                if (pid == 0) {
                    sigset_t empty; sigemptyset(&empty); sigprocmask(SIG_SETMASK,&empty,nullptr);
                    setsid();
                    setup_child_context(unit);
                    std::vector<const char*> argv;
                    argv.push_back(ep.c_str());
                    for (auto& a : args) argv.push_back(a.c_str());
                    argv.push_back(nullptr);
                    execvp(ep.c_str(), const_cast<char**>(argv.data()));
                    _exit(127);
                }
                if (pid > 0) {
                    int st = 0; ::waitpid(pid, &st, 0);
                    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
                    if (code != 0 && !ignore) {
                        s.log_fmt(LogLevel::Error,"executor",
                            "{}: ExecStartPre failed ({}), skipping service",name,code);
                        return;
                    }
                }
            }
        }

        if (unit.exec_start.empty()) {
            s.log_fmt(LogLevel::Debug,"executor","{}: no ExecStart, skipping",name);
            return;
        }

        std::string cmd = unit.exec_start;
        bool ignore_fail = false;
        if (!cmd.empty() && cmd[0]=='-') { ignore_fail=true; cmd=cmd.substr(1); }

        SpecifierContext ctx(name, unit.path);
        cmd = spec_resolver_.resolve(cmd, ctx);
        auto args = split_exec(cmd);
        if (args.empty()) return;
        std::string exec_path = args[0]; args.erase(args.begin());
        for (auto& a : args) a = spec_resolver_.resolve(a, ctx);

        auto policy      = parse_restart_policy(unit.restart_policy);
        int  burst_limit = unit.restart_limit_burst;
        int  burst_ivl   = unit.restart_limit_interval;

        // Socket activation path
        if constexpr (requires { s.collect_socket_env(name); }) {
            auto env_opt = s.collect_socket_env(name);
            if (env_opt) {
                s.log_fmt(LogLevel::Info,"executor",
                    "Socket-activating {} ({} fd(s))",name,env_opt->fds.size());
                bool rf = (policy != RestartPolicy::No);
                pid_t pid = s.spawn_with_socket_fds(name,exec_path,args,*env_opt,rf,&unit);
                if (pid > 0) {
                    s.apply_restart_count(pid, name);
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

        s.log_fmt(LogLevel::Info,"executor","Starting service: {} ({})",name,exec_path);
        pid_t pid = s.spawn(name, exec_path, args, false, policy, burst_limit, burst_ivl, &unit);
        if (pid > 0) s.apply_restart_count(pid, name);
        else if (!ignore_fail)
            s.log_fmt(LogLevel::Error,"executor","Failed to start {}",name);
    }

    // ----------------------------------------------------------------
    void start_mount(const std::string& name, const UnitFile& unit) {
        auto& s = this->self();
        if (unit.what.empty() || unit.where.empty()) {
            s.log_fmt(LogLevel::Warning,"executor","{}: incomplete mount unit",name); return;
        }
        s.log_fmt(LogLevel::Info,"executor","Mounting {} on {}",unit.what,unit.where);
        ::mkdir(unit.where.c_str(), 0755);
        auto [flags, data] = detail::parse_mount_options(unit.options);
        int ret = ::mount(unit.what.c_str(), unit.where.c_str(),
                          unit.mount_type.empty() ? nullptr : unit.mount_type.c_str(),
                          flags,
                          data.empty() ? nullptr : data.c_str());
        if (ret != 0)
            s.log_fmt(LogLevel::Error,"executor","mount {} failed: {}",name,strerror(errno));
    }

    // ----------------------------------------------------------------
    void process_restarts() {
        auto& s = this->self();
        for (const auto& name : s.drain_pending_restarts()) {
            const auto* unit = s.find_unit(name);
            if (unit && unit->type == UnitType::Service) {
                s.log_fmt(LogLevel::Notice,"executor","Restarting {}",name);
                start_service(name, *unit);
            }
        }
    }

    void queue_restart(const std::string& unit_name) {
        auto& s = this->self();
        s.pending_restarts_.push_back({unit_name, std::chrono::steady_clock::now(), 0, 0, {}});
    }

private:
    static std::vector<std::string> split_exec(const std::string& s) {
        std::vector<std::string> r; std::string t; bool iq=false; char qc=0;
        for (char c:s) {
            if(iq){if(c==qc)iq=false;else t+=c;}
            else if(c=='"'||c=='\''){iq=true;qc=c;}
            else if(c==' '||c=='\t'){if(!t.empty()){r.push_back(t);t.clear();}}
            else t+=c;
        }
        if(!t.empty())r.push_back(t); return r;
    }
};

} // namespace tinit