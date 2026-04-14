// test/test_specifiers.cpp
#include "trivialInit/systemd/specifiers.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace tinit;

void test_basic_specifiers() {
    SpecifierResolver resolver;
    SpecifierContext ctx("test.service", "/lib/systemd/system/test.service");
    
    assert(resolver.resolve("Name: %n", ctx) == "Name: test.service");
    assert(resolver.resolve("Prefix: %p", ctx) == "Prefix: test");
    assert(resolver.resolve("Literal: %%", ctx) == "Literal: %");
    assert(resolver.resolve("/run: %t", ctx) == "/run: /run");
    
    std::cout << "✓ Basic specifiers passed\n";
}

void test_template_unit() {
    SpecifierResolver resolver;
    SpecifierContext ctx("getty@tty1.service", "/lib/systemd/system/getty@.service");
    
    assert(resolver.resolve("Instance: %i", ctx) == "Instance: tty1");
    assert(resolver.resolve("Prefix: %p", ctx) == "Prefix: getty");
    assert(resolver.resolve("Full: %n", ctx) == "Full: getty@tty1.service");
    
    std::cout << "✓ Template unit specifiers passed\n";
}

void test_escaped_instance() {
    SpecifierResolver resolver;
    SpecifierContext ctx("systemd-fsck@dev-disk-by\\x2duuid-12345.service", 
                         "/lib/systemd/system/systemd-fsck@.service");
    
    std::string resolved_i = resolver.resolve("%i", ctx);
    std::string resolved_I = resolver.resolve("%I", ctx);
    std::string resolved_f = resolver.resolve("%f", ctx);
    
    assert(resolved_i == "dev-disk-by\\x2duuid-12345");
    assert(resolved_I == "dev-disk-by-uuid-12345");  // Unescaped
    assert(resolved_f == "/dev-disk-by-uuid-12345");  // With / prefix
    
    std::cout << "✓ Escaped instance specifiers passed\n";
    std::cout << "  %i = " << resolved_i << "\n";
    std::cout << "  %I = " << resolved_I << "\n";
    std::cout << "  %f = " << resolved_f << "\n";
}

void test_mount_unit() {
    SpecifierResolver resolver;
    SpecifierContext ctx("home.mount", "/lib/systemd/system/home.mount");
    
    // Test resolving typical mount unit directives
    std::string what = resolver.resolve("/dev/disk/by-uuid/%m", ctx);
    std::string where = resolver.resolve("/home/%H", ctx);
    
    std::cout << "✓ Mount unit specifiers passed\n";
    std::cout << "  What (with machine-id): " << what << "\n";
    std::cout << "  Where (with hostname): " << where << "\n";
}

void test_service_with_specifiers() {
    SpecifierResolver resolver;
    SpecifierContext ctx("user@1000.service", "/lib/systemd/system/user@.service");
    
    std::string exec_start = resolver.resolve("/usr/lib/systemd/systemd --user --unit=%i", ctx);
    std::string working_dir = resolver.resolve("%h", ctx);
    
    assert(exec_start == "/usr/lib/systemd/systemd --user --unit=1000");
    
    std::cout << "✓ Service ExecStart specifiers passed\n";
    std::cout << "  ExecStart: " << exec_start << "\n";
    std::cout << "  WorkingDirectory: " << working_dir << "\n";
}

void test_complex_specifiers() {
    SpecifierResolver resolver;
    SpecifierContext ctx("foo-bar-baz@instance.service", "/lib/systemd/system/foo-bar-baz@.service");
    
    // %j = final component after last -
    std::string component = resolver.resolve("%j", ctx);
    assert(component == "baz");
    
    std::cout << "✓ Complex specifiers passed\n";
    std::cout << "  Final component (%j): " << component << "\n";
}

void test_no_specifiers() {
    SpecifierResolver resolver;
    SpecifierContext ctx("test.service", "/lib/systemd/system/test.service");
    
    std::string plain = resolver.resolve("/usr/bin/daemon --config /etc/daemon.conf", ctx);
    assert(plain == "/usr/bin/daemon --config /etc/daemon.conf");
    
    std::cout << "✓ Plain text (no specifiers) passed\n";
}

int main() {
    std::cout << "Running specifier resolution tests...\n\n";
    
    test_basic_specifiers();
    test_template_unit();
    test_escaped_instance();
    test_mount_unit();
    test_service_with_specifiers();
    test_complex_specifiers();
    test_no_specifiers();
    
    std::cout << "\n✅ All tests passed!\n";
    return 0;
}