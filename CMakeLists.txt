cmake_minimum_required(VERSION 3.25)
project(trivialInit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TINIT_BUILD_TESTS "Build tests" ON)
option(TINIT_STATIC "Static link for initramfs" OFF)
option(TINIT_NO_TUI "Build without ncurses TUI" OFF)

if(TINIT_STATIC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    # Static builds for initramfs don't need TUI (PID 1 never uses it)
    set(TINIT_NO_TUI ON)
endif()

# ncurses for TUI (optional)
if(NOT TINIT_NO_TUI)
    # Temporarily restore default suffixes so we find shared ncurses
    # even if TINIT_STATIC set .a-only (we don't static-link ncurses)
    set(_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".so;.a")
    find_package(Curses QUIET)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_saved_suffixes})
    if(NOT CURSES_FOUND)
        message(WARNING "ncurses not found — building without TUI.")
        set(TINIT_NO_TUI ON)
    endif()
endif()

add_library(trivialInit_core STATIC
    src/systemd/unit_parser.cpp
    src/systemd/unit_scanner.cpp
    src/systemd/unit_executor.cpp
    src/systemd/specifiers.cpp
    src/init/filesystem.cpp
    src/init/process.cpp
    src/init/signal_handler.cpp
    src/tui/tui_engine.cpp
    src/tui/status_view.cpp
    src/tui/journal_view.cpp
)

target_include_directories(trivialInit_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(NOT TINIT_NO_TUI)
    target_include_directories(trivialInit_core PUBLIC ${CURSES_INCLUDE_DIRS})
    target_link_libraries(trivialInit_core PUBLIC ${CURSES_LIBRARIES})
else()
    target_compile_definitions(trivialInit_core PUBLIC TINIT_NO_TUI=1)
endif()

# Main init binary
add_executable(trivialInit src/main.cpp)
target_link_libraries(trivialInit PRIVATE trivialInit_core)

# TUI monitor (runs as non-PID1 to inspect state)
if(NOT TINIT_NO_TUI)
    add_executable(trivialctl src/trivialctl.cpp)
    target_link_libraries(trivialctl PRIVATE trivialInit_core)
endif()

if(TINIT_BUILD_TESTS)
    enable_testing()
    add_executable(test_unit_parser test/test_unit_parser.cpp)
    target_link_libraries(test_unit_parser PRIVATE trivialInit_core)
    add_test(NAME unit_parser COMMAND test_unit_parser)

    add_executable(test_mixin_composition test/test_mixin_composition.cpp)
    target_link_libraries(test_mixin_composition PRIVATE trivialInit_core)
    add_test(NAME mixin_composition COMMAND test_mixin_composition)
endif()

# Add test executable:
add_executable(test_specifiers test/test_specifiers.cpp src/systemd/specifiers.cpp)
target_include_directories(test_specifiers PRIVATE include)
add_test(NAME test_specifiers COMMAND test_specifiers)