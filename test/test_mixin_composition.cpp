#include "trivialInit/init_system.hpp"
#include <cassert>
#include <cstdio>
#include <filesystem>

// Test 1: Verify the composed type has all mixin capabilities
void test_composition() {
    tinit::InitSystem sys;

    // JournalMixin
    sys.log(tinit::LogLevel::Info, "test", "composition test");
    assert(sys.entries().size() == 1);
    assert(sys.entries()[0].source == "test");

    // ProcessMixin — unit_to_pid_ should be accessible
    assert(sys.active_count() == 0);

    // UnitScanMixin — unit_paths() available
    assert(sys.unit_paths().empty());

    // UnitParseMixin — find_unit returns nullptr for unknown
    assert(sys.find_unit("nonexistent.service") == nullptr);

    // UnitExecMixin — exec_order_ accessible
    assert(sys.exec_order_.empty());

    std::printf("test_composition: PASS\n");
}

// Test 2: Scan + Parse pipeline on test fixtures
void test_scan_parse_pipeline() {
    namespace fs = std::filesystem;
    fs::create_directories("/tmp/tinit_test_compose/etc/systemd/system");

    // Write some test units
    {
        std::ofstream f("/tmp/tinit_test_compose/etc/systemd/system/basic.target");
        f << "[Unit]\nDescription=Basic System\n";
    }
    {
        std::ofstream f("/tmp/tinit_test_compose/etc/systemd/system/network.target");
        f << "[Unit]\nDescription=Network\nAfter=basic.target\n";
    }
    {
        std::ofstream f("/tmp/tinit_test_compose/etc/systemd/system/sshd.service");
        f << "[Unit]\nDescription=OpenSSH\nAfter=network.target\n"
          << "[Service]\nType=simple\nExecStart=/usr/sbin/sshd -D\nRestart=on-failure\n"
          << "[Install]\nWantedBy=multi-user.target\n";
    }

    // Manually override search paths for test
    // (In production, kUnitSearchPaths is used. Here we test the parser directly.)
    tinit::InitSystem sys;

    // Manually feed paths
    for (const auto& entry : fs::directory_iterator("/tmp/tinit_test_compose/etc/systemd/system")) {
        sys.discovered_paths_.push_back(entry.path().string());
        sys.discovered_names_.insert(entry.path().filename().string());
    }

    // Parse
    static_cast<tinit::UnitParseMixin<tinit::InitSystem>&>(sys).execute(tinit::phase::UnitParse{});
    assert(sys.all_units().size() == 3);

    auto* sshd = sys.find_unit("sshd.service");
    assert(sshd != nullptr);
    assert(sshd->description == "OpenSSH");
    assert(sshd->exec_start == "/usr/sbin/sshd -D");
    assert(sshd->after.size() == 1);
    assert(sshd->after[0] == "network.target");

    // Resolve dependencies
    static_cast<tinit::UnitExecMixin<tinit::InitSystem>&>(sys).execute(tinit::phase::DependencyResolve{});
    assert(sys.exec_order_.size() == 3);

    // basic.target should come before network.target, which should come before sshd.service
    auto pos = [&](const std::string& name) -> int {
        for (int i = 0; i < (int)sys.exec_order_.size(); ++i)
            if (sys.exec_order_[i] == name) return i;
        return -1;
    };

    assert(pos("basic.target") < pos("network.target"));
    assert(pos("network.target") < pos("sshd.service"));

    std::printf("test_scan_parse_pipeline: PASS\n");
    fs::remove_all("/tmp/tinit_test_compose");
}

// Test 3: Journal ordering and levels
void test_journal() {
    tinit::InitSystem sys;
    sys.min_level_ = tinit::LogLevel::Debug;

    sys.log(tinit::LogLevel::Debug, "a", "debug msg");
    sys.log(tinit::LogLevel::Info, "b", "info msg");
    sys.log(tinit::LogLevel::Error, "c", "error msg");

    assert(sys.entries().size() == 3);
    assert(sys.entries()[0].level == tinit::LogLevel::Debug);
    assert(sys.entries()[1].level == tinit::LogLevel::Info);
    assert(sys.entries()[2].level == tinit::LogLevel::Error);

    std::printf("test_journal: PASS\n");
}

int main() {
    test_composition();
    test_scan_parse_pipeline();
    test_journal();
    std::printf("\nAll mixin composition tests passed.\n");
    return 0;
}
