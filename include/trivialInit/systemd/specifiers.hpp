// include/trivialInit/systemd/specifiers.hpp
#pragma once

#include <string>
#include <functional>
#include <unordered_map>

namespace tinit {

/// Context for resolving systemd unit specifiers
struct SpecifierContext {
    std::string unit_name;
    std::string unit_path;
    
    // Computed fields
    std::string unit_type;      // "service", "target", "mount"
    std::string prefix;         // "foo" from "foo@bar.service"
    std::string instance;       // "bar" from "foo@bar.service"
    
    // System state (lazy-loaded)
    mutable std::string boot_id_;
    mutable std::string machine_id_;
    mutable std::string hostname_;
    mutable std::string architecture_;
    
    explicit SpecifierContext(const std::string& name, const std::string& path = "");
    
    const std::string& boot_id() const;
    const std::string& machine_id() const;
    const std::string& hostname() const;
    const std::string& architecture() const;
};

/// Resolves systemd specifiers (%i, %n, %p, etc.) in unit file directives
class SpecifierResolver {
public:
    SpecifierResolver();
    
    /// Resolve all specifiers in input string using provided context
    std::string resolve(const std::string& input, const SpecifierContext& ctx) const;
    
private:
    using ResolverFunc = std::function<std::string(const SpecifierContext&)>;
    std::unordered_map<char, ResolverFunc> resolvers_;
    
    void register_core_resolvers();
};

/// Unescape systemd unit name encoding (\xNN hex escapes)
std::string unit_name_unescape(const std::string& s);

} // namespace tinit