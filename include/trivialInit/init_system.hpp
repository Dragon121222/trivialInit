#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/mixins/journal.hpp"
#include "trivialInit/mixins/filesystem.hpp"
#include "trivialInit/mixins/signal_handler.hpp"
#include "trivialInit/mixins/process.hpp"
#include "trivialInit/mixins/unit_scan.hpp"
#include "trivialInit/mixins/unit_parse.hpp"
#include "trivialInit/mixins/socket.hpp"
#include "trivialInit/mixins/unit_exec.hpp"
#include "trivialInit/mixins/tui.hpp"

namespace tinit {

class InitSystem : public MixinCompose<
    InitSystem,
    JournalMixin,
    FilesystemMixin,
    SignalMixin,
    ProcessMixin,
    UnitScanMixin,
    UnitParseMixin,
    SocketMixin,
    UnitExecMixin,
    TuiMixin
> {
public:
    using Fs    = FilesystemMixin<InitSystem>;
    using Sig   = SignalMixin<InitSystem>;
    using Scan  = UnitScanMixin<InitSystem>;
    using Parse = UnitParseMixin<InitSystem>;
    using Sock  = SocketMixin<InitSystem>;
    using Exec  = UnitExecMixin<InitSystem>;
    using Tui   = TuiMixin<InitSystem>;

    int run() {
        log(LogLevel::Info, "init", "trivialInit starting");

        int rc;
        if ((rc = Fs::execute(phase::EarlyMount{})))          return rc;
        if ((rc = Sig::execute(phase::SignalSetup{})))         return rc;
        if ((rc = Scan::execute(phase::UnitDiscovery{})))      return rc;
        if ((rc = Parse::execute(phase::UnitParse{})))         return rc;
        if ((rc = Sock::execute(phase::SocketBind{})))         return rc;
        if ((rc = Exec::execute(phase::DependencyResolve{})))  return rc;
        if ((rc = Exec::execute(phase::UnitExecute{})))        return rc;

        Sock::register_accept_sockets(epoll_fd_);
        Tui::execute(phase::TuiStart{});

        log(LogLevel::Info, "init", "Entering main loop");

        while (!shutdown_requested_) {
            poll_once_with_sockets(tui_active_ ? 100 : 1000);
            process_restarts();
            maybe_fix_runtime_dirs();

            if (tui_active_) {
                render();
                if (!handle_input()) {
                    if (getpid() != 1) break;
                }
            }
        }

        log(LogLevel::Info, "init", "Shutting down");
        shutdown_tui();

        for (auto it = exec_order_.rbegin(); it != exec_order_.rend(); ++it)
            stop_unit(*it);

        for (int i = 0; i < 50 && active_count() > 0; ++i)
            poll_once(100);

        for (auto& [pid, tp] : processes_)
            ::kill(pid, SIGKILL);
        poll_once(200);

        Sock::close_all_sockets();
        Fs::execute(phase::Shutdown{});

        log(LogLevel::Info, "init", "Goodbye.");
        return 0;
    }

    int run_monitor() {
        log(LogLevel::Info, "monitor", "trivialInit monitor starting");
        Scan::execute(phase::UnitDiscovery{});
        Parse::execute(phase::UnitParse{});
        Exec::execute(phase::DependencyResolve{});
#ifndef TINIT_NO_TUI
        init_tui();
        while (true) { render(); if (!handle_input()) break; napms(100); }
        shutdown_tui();
#else
        for (const auto& name : exec_order_) {
            const auto* u = find_unit(name);
            if (u) log_fmt(LogLevel::Info,"monitor","  {} — {}",name,u->description);
        }
#endif
        return 0;
    }

private:
    int poll_once_with_sockets(int timeout_ms) {
        struct epoll_event events[16];
        int n = epoll_wait(epoll_fd_, events, 16, timeout_ms);
        if (n < 0 && errno != EINTR) return -1;
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == signal_fd_)
                drain_signals();
            else if (fd_to_socket_.contains(fd))
                Sock::accept_ready(fd);
        }
        return n;
    }

    /// Once systemd-sysusers exits (it's no longer in unit_to_pid_),
    /// re-chown runtime directories with the now-populated user database.
    void maybe_fix_runtime_dirs() {
        if (runtime_dirs_chowned_) return;
        // Check sysusers has been started and has since exited
        bool was_started = false;
        for (auto& n : exec_order_)
            if (n == "systemd-sysusers.service") { was_started = true; break; }
        if (!was_started) return;
        if (unit_to_pid_.contains("systemd-sysusers.service")) return;
        // It ran and exited — apply ownership
        log(LogLevel::Info, "fs", "Applying deferred runtime directory ownership");
        Fs::fix_runtime_dir_ownership();
    }
};

} // namespace tinit