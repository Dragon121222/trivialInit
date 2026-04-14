#pragma once
/// trivialInit mixin architecture
///
/// Design: Each policy mixin is a CRTP layer parameterized on the final
/// Derived type. MixinBase is tagged per-mixin to avoid diamond ambiguity.
/// MixinCompose does plain variadic inheritance — phase dispatch is
/// explicit in InitSystem, not via using-pack (which fails for mixins
/// that don't define execute()).

#include <type_traits>
#include <concepts>

namespace tinit {

/// CRTP base — tagged by MixinTag to avoid diamond inheritance.
/// Each mixin passes itself as Tag: struct FooMixin : MixinBase<Derived, FooMixin>
template <typename Derived, typename MixinTag>
struct MixinBase {
    constexpr Derived& self() noexcept {
        return static_cast<Derived&>(*this);
    }
    constexpr const Derived& self() const noexcept {
        return static_cast<const Derived&>(*this);
    }
};

/// Tag types for init phases
namespace phase {
    struct EarlyMount {};
    struct SignalSetup {};
    struct UnitDiscovery {};
    struct UnitParse {};
    struct DependencyResolve {};
    struct UnitExecute {};
    struct TuiStart {};
    struct Reap {};
    struct Shutdown {};
}

/// Mixin combiner — simple variadic inheritance
template <typename Derived, template<typename> class... Mixins>
struct MixinCompose : Mixins<Derived>... {};

} // namespace tinit
