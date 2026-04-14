#pragma once

#include "trivialInit/mixins/mixin_base.hpp"
#include "trivialInit/mixins/process.hpp"
#include "trivialInit/mixins/journal.hpp"

#ifdef TINIT_NO_TUI

// Stub TUI mixin — compiles to no-ops when ncurses isn't available
#include <string>
#include <cstdint>
#include <unistd.h>

namespace tinit {

template <typename Derived>
struct TuiMixin : MixinBase<Derived, TuiMixin<Derived>> {
    bool tui_active_ = false;

    int execute(phase::TuiStart) { return 0; }
    int init_tui() { return 0; }
    void render() {}
    bool handle_input() { return true; }
    void shutdown_tui() {}
};

} // namespace tinit

#else // Full TUI implementation

#include <ncurses.h>
#include <string>
#include <algorithm>

namespace tinit {

enum class TuiTab : uint8_t {
    Overview  = 0,
    Services  = 1,
    Journal   = 2,
};

template <typename Derived>
struct TuiMixin : MixinBase<Derived, TuiMixin<Derived>> {
    WINDOW* main_win_   = nullptr;
    WINDOW* status_win_ = nullptr;
    WINDOW* log_win_    = nullptr;
    WINDOW* header_win_ = nullptr;
    bool tui_active_ = false;
    TuiTab current_tab_ = TuiTab::Overview;
    int scroll_offset_ = 0;
    int selected_row_ = 0;
    int svc_scroll_ = 0;

    int execute(phase::TuiStart) {
        // TUI is optional — only start if stdout is a tty
        // and we're not PID 1 (PID 1 shouldn't grab the console for TUI)
        if (getpid() == 1) return 0;

        auto& s = this->self();
        s.log(LogLevel::Info, "tui", "Initializing TUI");
        return init_tui();
    }

    int init_tui() {
        initscr();
        cbreak();
        noecho();
        curs_set(0);
        keypad(stdscr, TRUE);
        nodelay(stdscr, TRUE);
        start_color();
        use_default_colors();

        init_pair(1, COLOR_GREEN,   -1);  // running
        init_pair(2, COLOR_RED,     -1);  // failed
        init_pair(3, COLOR_YELLOW,  -1);  // starting
        init_pair(4, COLOR_CYAN,    -1);  // headers
        init_pair(5, COLOR_WHITE,   -1);  // normal
        init_pair(6, COLOR_MAGENTA, -1);  // stopped
        init_pair(7, COLOR_BLACK, COLOR_CYAN); // selected

        create_windows();
        tui_active_ = true;
        return 0;
    }

    void create_windows() {
        int h, w;
        getmaxyx(stdscr, h, w);

        header_win_ = newwin(3, w, 0, 0);
        main_win_   = newwin(h - 6, w, 3, 0);
        status_win_ = newwin(3, w, h - 3, 0);
    }

    void render() {
        if (!tui_active_) return;

        int h, w;
        getmaxyx(stdscr, h, w);

        render_header(w);

        switch (current_tab_) {
        case TuiTab::Overview:  render_overview(h - 6, w); break;
        case TuiTab::Services:  render_services(h - 6, w); break;
        case TuiTab::Journal:   render_journal(h - 6, w);  break;
        }

        render_status_bar(w);

        doupdate();
    }

    void render_header(int w) {
        werase(header_win_);
        wattron(header_win_, COLOR_PAIR(4) | A_BOLD);
        mvwprintw(header_win_, 0, 1, "trivialInit v0.1");
        wattroff(header_win_, A_BOLD);

        // Tab bar
        const char* tabs[] = {"[1] Overview", "[2] Services", "[3] Journal"};
        int x = 2;
        for (int i = 0; i < 3; ++i) {
            if (static_cast<int>(current_tab_) == i) {
                wattron(header_win_, A_REVERSE);
            }
            mvwprintw(header_win_, 1, x, "%s", tabs[i]);
            if (static_cast<int>(current_tab_) == i) {
                wattroff(header_win_, A_REVERSE);
            }
            x += static_cast<int>(strlen(tabs[i])) + 3;
        }

        box(header_win_, 0, 0);
        wnoutrefresh(header_win_);
    }

    void render_overview(int h, int w) {
        auto& s = this->self();
        werase(main_win_);

        int y = 1;
        wattron(main_win_, COLOR_PAIR(4) | A_BOLD);
        mvwprintw(main_win_, y++, 2, "System Status");
        wattroff(main_win_, A_BOLD);
        y++;

        // Count states
        int running = 0, failed = 0, stopped = 0, total = 0;
        for (const auto& [name, unit] : s.all_units()) {
            total++;
            auto pit = s.unit_to_pid_.find(name);
            if (pit != s.unit_to_pid_.end()) running++;
        }

        mvwprintw(main_win_, y++, 2, "Units loaded:   %d", total);
        wattron(main_win_, COLOR_PAIR(1));
        mvwprintw(main_win_, y++, 2, "Active:         %d", s.active_count());
        wattroff(main_win_, COLOR_PAIR(1));
        mvwprintw(main_win_, y++, 2, "PID:            %d", getpid());

        y += 2;
        wattron(main_win_, COLOR_PAIR(4) | A_BOLD);
        mvwprintw(main_win_, y++, 2, "Recent Log");
        wattroff(main_win_, A_BOLD);
        y++;

        const auto& entries = s.entries();
        int start = std::max(0, static_cast<int>(entries.size()) - (h - y - 1));
        for (int i = start; i < static_cast<int>(entries.size()) && y < h - 1; ++i) {
            const auto& e = entries[i];
            int color = (e.level >= LogLevel::Error) ? 2 :
                        (e.level >= LogLevel::Warning) ? 3 : 5;
            wattron(main_win_, COLOR_PAIR(color));
            mvwprintw(main_win_, y++, 2, "[%s] %.*s",
                e.source.c_str(),
                std::min(w - 20, static_cast<int>(e.message.size())),
                e.message.c_str());
            wattroff(main_win_, COLOR_PAIR(color));
        }

        box(main_win_, 0, 0);
        wnoutrefresh(main_win_);
    }

    void render_services(int h, int w) {
        auto& s = this->self();
        werase(main_win_);

        int y = 1;
        wattron(main_win_, COLOR_PAIR(4) | A_BOLD);
        mvwprintw(main_win_, y, 2, "%-30s %-10s %-10s %s", "UNIT", "TYPE", "STATE", "DESCRIPTION");
        wattroff(main_win_, A_BOLD);
        y++;

        int visible_rows = h - 3; // header row + box borders
        int total = static_cast<int>(s.all_units().size());

        // Keep selected_row_ in viewport
        if (selected_row_ < svc_scroll_) svc_scroll_ = selected_row_;
        if (selected_row_ >= svc_scroll_ + visible_rows) svc_scroll_ = selected_row_ - visible_rows + 1;

        int row = 0;
        for (const auto& [name, unit] : s.all_units()) {
            if (row < svc_scroll_) { row++; continue; }
            if (y >= h - 1) break;

            // Determine state
            const char* state = "inactive";
            int color = 5;
            auto pit = s.unit_to_pid_.find(name);
            if (pit != s.unit_to_pid_.end()) {
                state = "running";
                color = 1;
            } else if (unit.type == UnitType::Target) {
                state = "reached";
                color = 1;
            }

            if (row == selected_row_) {
                wattron(main_win_, COLOR_PAIR(7));
            } else {
                wattron(main_win_, COLOR_PAIR(color));
            }

            const char* type_s = (unit.type == UnitType::Service) ? "service" :
                                 (unit.type == UnitType::Target) ? "target" :
                                 (unit.type == UnitType::Mount) ? "mount" : "unknown";

            mvwprintw(main_win_, y, 2, "%-30s %-10s %-10s %.*s",
                name.c_str(), type_s, state,
                std::min(w - 56, static_cast<int>(unit.description.size())),
                unit.description.c_str());

            if (row == selected_row_) {
                wattroff(main_win_, COLOR_PAIR(7));
            } else {
                wattroff(main_win_, COLOR_PAIR(color));
            }

            y++;
            row++;
        }

        // Scroll indicator
        if (total > visible_rows) {
            mvwprintw(main_win_, h - 2, w - 12, " %d/%d ", selected_row_ + 1, total);
        }

        box(main_win_, 0, 0);
        wnoutrefresh(main_win_);
    }

    void render_journal(int h, int w) {
        auto& s = this->self();
        werase(main_win_);

        const auto& entries = s.entries();
        int visible = h - 2;
        int total = static_cast<int>(entries.size());
        int start = std::max(0, total - visible - scroll_offset_);
        int end = std::min(total, start + visible);

        int y = 1;
        for (int i = start; i < end; ++i) {
            const auto& e = entries[i];
            int color = (e.level >= LogLevel::Error) ? 2 :
                        (e.level >= LogLevel::Warning) ? 3 : 5;
            const char* lvl[] = {"DBG", "INF", "NTC", "WRN", "ERR", "CRT"};

            wattron(main_win_, COLOR_PAIR(color));
            mvwprintw(main_win_, y, 1, " %s %-12s %.*s",
                lvl[static_cast<int>(e.level)],
                e.source.c_str(),
                std::min(w - 20, static_cast<int>(e.message.size())),
                e.message.c_str());
            wattroff(main_win_, COLOR_PAIR(color));
            y++;
        }

        box(main_win_, 0, 0);
        wnoutrefresh(main_win_);
    }

    void render_status_bar(int w) {
        werase(status_win_);
        wattron(status_win_, COLOR_PAIR(4));
        mvwprintw(status_win_, 1, 2,
            "q:Quit  1-3:Tabs  j/k:Scroll  s:Start  x:Stop  r:Restart");
        wattroff(status_win_, COLOR_PAIR(4));
        box(status_win_, 0, 0);
        wnoutrefresh(status_win_);
    }

    /// Handle keyboard input, returns false if quit requested
    bool handle_input() {
        int ch = getch();
        if (ch == ERR) return true;

        auto& s = this->self();
        int total_units = static_cast<int>(s.all_units().size());

        switch (ch) {
        case 'q': case 'Q': return false;
        case '1': current_tab_ = TuiTab::Overview;  break;
        case '2': current_tab_ = TuiTab::Services;  break;
        case '3': current_tab_ = TuiTab::Journal;   break;
        case 'j': case KEY_DOWN:
            if (current_tab_ == TuiTab::Journal) {
                scroll_offset_ = std::max(0, scroll_offset_ - 1);
            } else {
                selected_row_ = std::min(selected_row_ + 1, std::max(0, total_units - 1));
            }
            break;
        case 'k': case KEY_UP:
            if (current_tab_ == TuiTab::Journal) {
                scroll_offset_++;
            } else {
                selected_row_ = std::max(0, selected_row_ - 1);
            }
            break;
        case KEY_RESIZE:
            endwin();
            refresh();
            delwin(header_win_);
            delwin(main_win_);
            delwin(status_win_);
            create_windows();
            break;
        }
        return true;
    }

    void shutdown_tui() {
        if (tui_active_) {
            delwin(header_win_);
            delwin(main_win_);
            delwin(status_win_);
            endwin();
            tui_active_ = false;
        }
    }

    ~TuiMixin() { shutdown_tui(); }
};

} // namespace tinit

#endif // TINIT_NO_TUI
