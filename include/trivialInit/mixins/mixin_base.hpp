#pragma once

#include <type_traits>
#include <concepts>

namespace tinit {

template <typename Derived, typename MixinTag>
struct MixinBase {
    constexpr Derived& self() noexcept {
        return static_cast<Derived&>(*this);
    }
    constexpr const Derived& self() const noexcept {
        return static_cast<const Derived&>(*this);
    }
};

namespace phase {
    struct EarlyMount {};
    struct SignalSetup {};
    struct UnitDiscovery {};
    struct UnitParse {};
    struct SocketBind {};
    struct DependencyResolve {};
    struct UnitExecute {};
    struct TuiStart {};
    struct Reap {};
    struct Shutdown {};
}

template <typename Derived, template<typename> class... Mixins>
struct MixinCompose : Mixins<Derived>... {};

} // namespace tinit