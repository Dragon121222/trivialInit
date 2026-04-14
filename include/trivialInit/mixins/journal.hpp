#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <format>

namespace tinit {

enum class LogLevel : uint8_t {
    Debug   = 0,
    Info    = 1,
    Notice  = 2,
    Warning = 3,
    Error   = 4,
    Crit    = 5,
};

struct JournalEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string source;   // which mixin/unit
    std::string message;
};

template <typename Derived>
struct JournalMixin : MixinBase<Derived, JournalMixin<Derived>> {
    std::vector<JournalEntry> journal_entries_;
    std::mutex journal_mutex_;
    LogLevel min_level_ = LogLevel::Info;

    void log(LogLevel level, std::string_view source, std::string_view msg) {
        if (level < min_level_) return;
        auto now = std::chrono::system_clock::now();
        std::lock_guard lock(journal_mutex_);
        journal_entries_.push_back({now, level, std::string(source), std::string(msg)});

        // Also write to kmsg/console when available
        const char* lvl_str[] = {"DBG", "INF", "NTC", "WRN", "ERR", "CRT"};
        std::fprintf(stderr, "[%s] %.*s: %.*s\n",
            lvl_str[static_cast<int>(level)],
            static_cast<int>(source.size()), source.data(),
            static_cast<int>(msg.size()), msg.data());
    }

    template <typename... Args>
    void log_fmt(LogLevel level, std::string_view source, std::format_string<Args...> fmt, Args&&... args) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        log(level, source, msg);
    }

    const std::vector<JournalEntry>& entries() const { return journal_entries_; }
};

} // namespace tinit
