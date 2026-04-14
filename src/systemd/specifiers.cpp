// src/systemd/specifiers.cpp
#include "trivialInit/systemd/specifiers.hpp"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <sys/utsname.h>
#include <cstring>

namespace tinit {

static std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (std::getline(f, line)) {
        line.erase(line.find_last_not_of(" \n\r\t") + 1);
        return line;
    }
    return "";
}

std::string unit_name_unescape(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i+1] == 'x' && 
            std::isxdigit(s[i+2]) && std::isxdigit(s[i+3])) {
            result += static_cast<char>(std::stoi(s.substr(i+2, 2), nullptr, 16));
            i += 3;
        } else {
            result += s[i];
        }
    }
    return result;
}

SpecifierContext::SpecifierContext(const std::string& name, const std::string& path)
    : unit_name(name), unit_path(path) {
    
    // Extract type
    auto dot_pos = unit_name.rfind('.');
    if (dot_pos != std::string::npos) {
        unit_type = unit_name.substr(dot_pos + 1);
    }
    
    // Extract prefix and instance for template units
    auto at_pos = unit_name.find('@');
    if (at_pos != std::string::npos && dot_pos != std::string::npos && at_pos < dot_pos) {
        prefix = unit_name.substr(0, at_pos);
        instance = unit_name.substr(at_pos + 1, dot_pos - at_pos - 1);
    } else if (dot_pos != std::string::npos) {
        prefix = unit_name.substr(0, dot_pos);
    } else {
        prefix = unit_name;
    }
}

const std::string& SpecifierContext::boot_id() const {
    if (boot_id_.empty()) {
        boot_id_ = read_first_line("/proc/sys/kernel/random/boot_id");
    }
    return boot_id_;
}

const std::string& SpecifierContext::machine_id() const {
    if (machine_id_.empty()) {
        machine_id_ = read_first_line("/etc/machine-id");
    }
    return machine_id_;
}

const std::string& SpecifierContext::hostname() const {
    if (hostname_.empty()) {
        char buf[256];
        if (gethostname(buf, sizeof(buf)) == 0) {
            hostname_ = buf;
            auto dot = hostname_.find('.');
            if (dot != std::string::npos) {
                hostname_ = hostname_.substr(0, dot);
            }
        }
    }
    return hostname_;
}

const std::string& SpecifierContext::architecture() const {
    if (architecture_.empty()) {
        struct utsname uts;
        if (uname(&uts) == 0) {
            architecture_ = uts.machine;
        }
    }
    return architecture_;
}

SpecifierResolver::SpecifierResolver() {
    register_core_resolvers();
}

void SpecifierResolver::register_core_resolvers() {
    // Literal percent
    resolvers_['%'] = [](const SpecifierContext&) { return "%"; };
    
    // Unit identifiers
    resolvers_['n'] = [](const SpecifierContext& c) { return c.unit_name; };
    resolvers_['N'] = [](const SpecifierContext& c) { return unit_name_unescape(c.unit_name); };
    resolvers_['p'] = [](const SpecifierContext& c) { return c.prefix; };
    resolvers_['P'] = [](const SpecifierContext& c) { return unit_name_unescape(c.prefix); };
    resolvers_['i'] = [](const SpecifierContext& c) { return c.instance; };
    resolvers_['I'] = [](const SpecifierContext& c) { return unit_name_unescape(c.instance); };
    
    // Instance with / prefix
    resolvers_['f'] = [](const SpecifierContext& c) { 
        return c.instance.empty() ? "" : "/" + unit_name_unescape(c.instance); 
    };
    
    // Final component after last -
    resolvers_['j'] = [](const SpecifierContext& c) {
        auto pos = c.prefix.rfind('-');
        return pos == std::string::npos ? c.prefix : c.prefix.substr(pos + 1);
    };
    resolvers_['J'] = [](const SpecifierContext& c) {
        auto pos = c.prefix.rfind('-');
        std::string comp = pos == std::string::npos ? c.prefix : c.prefix.substr(pos + 1);
        return unit_name_unescape(comp);
    };
    
    // Directories
    resolvers_['t'] = [](const SpecifierContext&) { return "/run"; };
    resolvers_['S'] = [](const SpecifierContext&) { return "/var/lib"; };
    resolvers_['C'] = [](const SpecifierContext&) { return "/var/cache"; };
    resolvers_['L'] = [](const SpecifierContext&) { return "/var/log"; };
    resolvers_['E'] = [](const SpecifierContext&) { return "/etc"; };
    
    // System IDs
    resolvers_['b'] = [](const SpecifierContext& c) { return c.boot_id(); };
    resolvers_['m'] = [](const SpecifierContext& c) { return c.machine_id(); };
    resolvers_['H'] = [](const SpecifierContext& c) { return c.hostname(); };
    resolvers_['a'] = [](const SpecifierContext& c) { return c.architecture(); };
    
    // Kernel version
    resolvers_['v'] = [](const SpecifierContext&) {
        struct utsname uts;
        return uname(&uts) == 0 ? std::string(uts.release) : "";
    };
    
    // OS release info (simplified - not reading /etc/os-release)
    resolvers_['o'] = [](const SpecifierContext&) { return ""; };  // OS ID
    resolvers_['w'] = [](const SpecifierContext&) { return ""; };  // OS Version ID
    resolvers_['W'] = [](const SpecifierContext&) { return ""; };  // OS Build ID
    
    // User info
    resolvers_['u'] = [](const SpecifierContext&) {
        char buf[256];
        return getlogin_r(buf, sizeof(buf)) == 0 ? std::string(buf) : "";
    };
    resolvers_['U'] = [](const SpecifierContext&) { return std::to_string(getuid()); };
    resolvers_['g'] = [](const SpecifierContext&) { return std::to_string(getgid()); };
    resolvers_['h'] = [](const SpecifierContext&) { 
        const char* home = getenv("HOME");
        return home ? home : ""; 
    };
    resolvers_['s'] = [](const SpecifierContext&) { 
        const char* shell = getenv("SHELL");
        return shell ? shell : "/bin/sh"; 
    };
}

std::string SpecifierResolver::resolve(const std::string& input, const SpecifierContext& ctx) const {
    std::string result;
    result.reserve(input.size() * 2);
    
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 1 < input.size()) {
            char spec = input[i + 1];
            auto it = resolvers_.find(spec);
            
            if (it != resolvers_.end()) {
                result += it->second(ctx);
                ++i;
            } else {
                // Unknown specifier - pass through as-is
                result += input[i];
                result += input[i + 1];
                ++i;
            }
        } else {
            result += input[i];
        }
    }
    
    return result;
}

} // namespace tinit