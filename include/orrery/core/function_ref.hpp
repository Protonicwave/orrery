#pragma once

/// \file
/// A non-owning reference to something callable.
///
/// The scheduler in `backend/` has to hand a piece of work to a thread it did
/// not compile against. The work is a lambda closing over whatever the caller
/// happened to have to hand, the scheduler is a virtual interface chosen at run
/// time, and a virtual function cannot be a template, so the closure has to be
/// erased behind a uniform type at that boundary.
///
/// `std::function` is the usual answer and is the wrong one here. It owns what
/// it holds, so constructing one from a closure that captures four spans may
/// allocate, and the boundary it would sit on is crossed once per force
/// evaluation: several times per timestep, for the whole length of a run. The
/// ownership buys nothing, because the closure provably outlives the call. It is
/// a local in the caller's frame and the callee returns before the caller does.
///
/// This is the type the standard adopted for the same reason, as
/// `std::function_ref` in C++26. The project is on C++20, so it is written here
/// rather than waited for, and it can be deleted when the compilers catch up.
///
/// The cost of not owning is the usual one, and it is real: a `FunctionRef`
/// outliving the callable it refers to is a dangling pointer with no diagnostic.
/// It is safe in the one shape this project uses it in, an argument to a call
/// that returns before the full expression ends, and it should not be stored in
/// a member or returned from a function.

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace orrery::core {

template<typename Signature> class FunctionRef;

/// A reference to a callable taking `Args` and returning `Result`.
///
/// Copying one is copying two pointers. It does not participate in the lifetime
/// of what it refers to, in the same way and for the same reasons `std::span`
/// does not participate in the lifetime of what it views.
template<typename Result, typename... Args> class FunctionRef<Result(Args...)> {
public:
    /// Refer to `callable`, which must outlive this reference.
    ///
    /// Implicit, so that a lambda can be written at the call site of a function
    /// taking one of these, which is the whole point of the type. The constraint
    /// excludes `FunctionRef` itself so that copy construction is not hijacked,
    /// and excludes const callables so that the erased pointer can be a plain
    /// `void*` and the thunk can recover it without a const cast.
    template<typename Callable, typename Erased = std::remove_reference_t<Callable>>
        requires(!std::is_same_v<Erased, FunctionRef> && !std::is_const_v<Erased> &&
                 std::is_invocable_r_v<Result, Erased&, Args...>)
    // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
    constexpr FunctionRef(Callable&& callable) noexcept
        : object_(std::addressof(callable)), invoke_(&invoke_erased<Erased>) {}

    constexpr Result operator()(Args... args) const {
        return invoke_(object_, std::forward<Args>(args)...);
    }

private:
    /// The one function per erased type that knows what the `void*` points at.
    template<typename Erased> static Result invoke_erased(void* object, Args... args) {
        return std::invoke(*static_cast<Erased*>(object), std::forward<Args>(args)...);
    }

    void* object_;
    Result (*invoke_)(void*, Args...);
};

} // namespace orrery::core
