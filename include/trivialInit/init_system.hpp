#pragma once

/// InitSystem — the final composed type
///
/// All mixin axes collapse into one class via MixinCompose.
/// Phase dispatch uses qualified calls: FilesystemMixin<InitSystem>::execute(...)
/// to avoid ambiguity from multiple inherited execute() overloads.

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/mixins/journal.hpp"
#include "trivialInit/mixins/filesystem.hpp"
#include "trivialInit/mixins/signal_handler.hpp"
#include "trivialInit/mixins/process.hpp"
#include "trivialInit/mixins/unit_scan.hpp"
#include "trivialInit/mixins/unit_parse.hpp"
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
    UnitExecMixin,
    TuiMixin
> {
public:
    // Aliases for qualified dispatch
    using Fs    = FilesystemMixin<InitSystem>;
    using Sig   = SignalMixin<InitSystem>;
    using Scan  = UnitScanMixin<InitSystem>;
    using Parse = UnitParseMixin<InitSystem>;
    using Exec  = UnitExecMixin<InitSystem>;
    using Tui   = TuiMixin<InitSystem>;

    /// Run the full init sequence
    int run() {
        log(LogLevel::Info, "init", "trivialInit starting");

        int rc;
        if ((rc = Fs::execute(phase::EarlyMount{})))           return rc;
        if ((rc = Sig::execute(phase::SignalSetup{})))          return rc;
        if ((rc = Scan::execute(phase::UnitDiscovery{})))       return rc;
        if ((rc = Parse::execute(phase::UnitParse{})))          return rc;
        if ((rc = Exec::execute(phase::DependencyResolve{})))   return rc;
        if ((rc = Exec::execute(phase::UnitExecute{})))         return rc;

        // TUI is optional, non-fatal
        Tui::execute(phase::TuiStart{});

        log(LogLevel::Info, "init", "Entering main loop");

        // Main loop
        while (!shutdown_requested_) {
            poll_once(tui_active_ ? 100 : 1000);
            process_restarts();

            if (tui_active_) {
                render();
                if (!handle_input()) {
                    if (getpid() != 1) break;
                }
            }
        }

        // Shutdown
        log(LogLevel::Info, "init", "Shutting down");
        shutdown_tui();

        // Stop all services in reverse order
        for (auto it = exec_order_.rbegin(); it != exec_order_.rend(); ++it) {
            stop_unit(*it);
        }

        // Wait for graceful exit
        for (int i = 0; i < 50 && active_count() > 0; ++i) {
            poll_once(100);
        }

        // SIGKILL stragglers
        for (auto& [pid, tp] : processes_) {
            ::kill(pid, SIGKILL);
        }
        poll_once(200);

        Fs::execute(phase::Shutdown{});

        log(LogLevel::Info, "init", "Goodbye.");
        return 0;
    }

    /// Run as monitor (not PID 1) — TUI only, no execution
    int run_monitor() {
        log(LogLevel::Info, "monitor", "trivialInit monitor starting");

        Scan::execute(phase::UnitDiscovery{});
        Parse::execute(phase::UnitParse{});
        Exec::execute(phase::DependencyResolve{});

#ifndef TINIT_NO_TUI
        init_tui();

        while (true) {
            render();
            if (!handle_input()) break;
            napms(100);
        }

        shutdown_tui();
#else
        log(LogLevel::Info, "monitor", "TUI disabled — printing unit summary");
        for (const auto& name : exec_order_) {
            const auto* u = find_unit(name);
            if (u) log_fmt(LogLevel::Info, "monitor", "  {} — {}", name, u->description);
        }
#endif
        return 0;
    }
};

} // namespace tinit
