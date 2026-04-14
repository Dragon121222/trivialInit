#include "trivialInit/mixins/unit_parse.hpp"
#include <cassert>
#include <fstream>
#include <filesystem>
#include <cstdio>

void write_test_unit(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

void test_parse_service() {
    namespace fs = std::filesystem;
    fs::create_directories("/tmp/tinit_test/systemd");

    write_test_unit("/tmp/tinit_test/systemd/test.service", R"(
[Unit]
Description=Test Service
After=network.target syslog.target
Requires=basic.target
Wants=logging.service

[Service]
Type=simple
ExecStart=/usr/bin/testd --daemon
ExecStop=/usr/bin/testd --stop
Restart=on-failure
User=nobody
WorkingDirectory=/var/lib/test
Environment=FOO=bar

[Install]
WantedBy=multi-user.target
)");

    auto sections = tinit::detail::parse_ini("/tmp/tinit_test/systemd/test.service");

    assert(sections.contains("Unit"));
    assert(sections.contains("Service"));
    assert(sections.contains("Install"));

    assert(sections["Unit"]["Description"] == "Test Service");
    assert(sections["Service"]["ExecStart"] == "/usr/bin/testd --daemon");
    assert(sections["Service"]["Restart"] == "on-failure");
    assert(sections["Service"]["User"] == "nobody");
    assert(sections["Install"]["WantedBy"] == "multi-user.target");

    // Test split_list
    auto after = tinit::detail::split_list(sections["Unit"]["After"]);
    assert(after.size() == 2);
    assert(after[0] == "network.target");
    assert(after[1] == "syslog.target");

    std::printf("test_parse_service: PASS\n");
    fs::remove_all("/tmp/tinit_test");
}

void test_parse_mount() {
    namespace fs = std::filesystem;
    fs::create_directories("/tmp/tinit_test/systemd");

    write_test_unit("/tmp/tinit_test/systemd/home.mount", R"(
[Unit]
Description=Home Directory
After=local-fs-pre.target

[Mount]
What=/dev/sda2
Where=/home
Type=ext4
Options=defaults,noatime
)");

    auto sections = tinit::detail::parse_ini("/tmp/tinit_test/systemd/home.mount");

    assert(sections["Mount"]["What"] == "/dev/sda2");
    assert(sections["Mount"]["Where"] == "/home");
    assert(sections["Mount"]["Type"] == "ext4");
    assert(sections["Mount"]["Options"] == "defaults,noatime");

    std::printf("test_parse_mount: PASS\n");
    fs::remove_all("/tmp/tinit_test");
}

void test_parse_target() {
    namespace fs = std::filesystem;
    fs::create_directories("/tmp/tinit_test/systemd");

    write_test_unit("/tmp/tinit_test/systemd/multi-user.target", R"(
[Unit]
Description=Multi-User System
Requires=basic.target
After=basic.target
Conflicts=rescue.service rescue.target
)");

    auto sections = tinit::detail::parse_ini("/tmp/tinit_test/systemd/multi-user.target");

    assert(sections["Unit"]["Description"] == "Multi-User System");
    auto conflicts = tinit::detail::split_list(sections["Unit"]["Conflicts"]);
    assert(conflicts.size() == 2);

    std::printf("test_parse_target: PASS\n");
    fs::remove_all("/tmp/tinit_test");
}

void test_repeated_keys() {
    namespace fs = std::filesystem;
    fs::create_directories("/tmp/tinit_test/systemd");

    write_test_unit("/tmp/tinit_test/systemd/multi.service", R"(
[Unit]
Description=Multi After Test
After=a.target
After=b.target
After=c.target
)");

    auto sections = tinit::detail::parse_ini("/tmp/tinit_test/systemd/multi.service");
    auto after = tinit::detail::split_list(sections["Unit"]["After"]);
    assert(after.size() == 3);
    assert(after[0] == "a.target");
    assert(after[1] == "b.target");
    assert(after[2] == "c.target");

    std::printf("test_repeated_keys: PASS\n");
    fs::remove_all("/tmp/tinit_test");
}

int main() {
    test_parse_service();
    test_parse_mount();
    test_parse_target();
    test_repeated_keys();
    std::printf("\nAll parser tests passed.\n");
    return 0;
}
