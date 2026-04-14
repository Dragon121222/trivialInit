#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace tinit {

template <typename Derived>
struct SignalMixin : MixinBase<Derived, SignalMixin<Derived>> {
    int signal_fd_ = -1;
    int epoll_fd_  = -1;
    bool shutdown_requested_ = false;

    int execute(phase::SignalSetup) {
        auto& s = this->self();
        s.log(LogLevel::Info, "signal", "Setting up signal handling");

        // Block all signals, handle via signalfd
        sigset_t mask;
        sigfillset(&mask);
        if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
            s.log_fmt(LogLevel::Crit, "signal", "sigprocmask: {}", strerror(errno));
            return 1;
        }

        // We want SIGCHLD (reap), SIGTERM/SIGINT (shutdown), SIGUSR1 (status)
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGUSR1);

        signal_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (signal_fd_ < 0) {
            s.log_fmt(LogLevel::Crit, "signal", "signalfd: {}", strerror(errno));
            return 1;
        }

        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            s.log_fmt(LogLevel::Crit, "signal", "epoll_create1: {}", strerror(errno));
            return 1;
        }

        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = signal_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, signal_fd_, &ev);

        s.log(LogLevel::Info, "signal", "Signal handling ready (signalfd + epoll)");
        return 0;
    }

    /// Process pending signals, returns number handled
    int drain_signals() {
        auto& s = this->self();
        struct signalfd_siginfo info{};
        int count = 0;

        while (true) {
            ssize_t n = ::read(signal_fd_, &info, sizeof(info));
            if (n != sizeof(info)) break;

            switch (info.ssi_signo) {
            case SIGCHLD:
                s.reap_children();
                ++count;
                break;
            case SIGTERM:
            case SIGINT:
                s.log(LogLevel::Notice, "signal", "Shutdown signal received");
                shutdown_requested_ = true;
                ++count;
                break;
            case SIGUSR1:
                // Status dump — TUI refresh trigger
                ++count;
                break;
            }
        }
        return count;
    }

    /// Main event loop tick — wait for signals with timeout
    int poll_once(int timeout_ms = 1000) {
        struct epoll_event events[8];
        int n = epoll_wait(epoll_fd_, events, 8, timeout_ms);
        if (n < 0 && errno != EINTR) return -1;
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == signal_fd_) {
                drain_signals();
            }
        }
        return n;
    }

    ~SignalMixin() {
        if (signal_fd_ >= 0) ::close(signal_fd_);
        if (epoll_fd_ >= 0)  ::close(epoll_fd_);
    }
};

} // namespace tinit
