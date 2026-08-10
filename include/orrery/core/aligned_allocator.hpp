#pragma once

/// \file
/// An allocator that starts every allocation on a cache line.
///
/// The target machine shares roughly 135 GB/s of memory bandwidth between its
/// CPU and its integrated GPU, and most kernels in this project are bound by
/// data movement rather than arithmetic. Two consequences follow from where an
/// array begins, and both are paid for by the kernel rather than by the
/// allocation.
///
/// A 256-bit AVX2 load reads 32 bytes. From an array whose first element sits
/// at an arbitrary offset, some of those loads straddle two 64-byte cache
/// lines, and a split load costs two line fills instead of one. Starting the
/// array on a line means an aligned load stays inside a line for the whole
/// array.
///
/// The threading in Phase 6 divides each component array into per-thread
/// ranges. Where two ranges share a cache line, the two threads writing them
/// contend for exclusive ownership of that line even though neither touches the
/// other's elements. Aligning the array is the first half of avoiding that; the
/// second half is choosing partition boundaries on line multiples, which is the
/// scheduler's job rather than the allocator's.
///
/// ADR-0005 records the alternatives.

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

namespace orrery::core {

/// The cache line length on the target machine, in bytes.
///
/// 64 on the Lion Cove and Skymont cores of Lunar Lake, as on every other
/// x86-64 part this project runs on. It is a constant rather than a query
/// because it has to be usable in a constant expression, and because a value
/// discovered at run time could not be relied on by anything compiled against
/// it.
inline constexpr std::size_t kCacheLineBytes = 64;

/// A standard allocator that returns cache-line-aligned storage.
///
/// The alignment is fixed rather than a template parameter. It is one decision
/// about one machine, and it is reviewed here once instead of at every
/// container that could otherwise ask for something different and be wrong
/// without anyone noticing. A fixed alignment also keeps the allocator in the
/// `Alloc<T>` shape that `std::allocator_traits` can rebind on its own, so this
/// class needs no rebind member of its own.
template<typename T> class AlignedAllocator {
public:
    using value_type = T;

    /// Moving a container moves its storage rather than copying its elements,
    /// which is the behaviour the containers of particle components need.
    using propagate_on_container_move_assignment = std::true_type;

    /// The allocator holds no state, so any instance can free memory that any
    /// other instance allocated.
    using is_always_equal = std::true_type;

    static constexpr std::size_t kAlignment = kCacheLineBytes;

    static_assert(kAlignment >= alignof(T),
                  "The requested alignment is weaker than the type's own requirement");

    AlignedAllocator() noexcept = default;

    /// Containers rebind an allocator to their internal types, so an allocator
    /// for one type has to be constructible from an allocator for another.
    template<typename U>
    constexpr AlignedAllocator(const AlignedAllocator<U>& /*other*/) noexcept {}

    /// Allocate storage for `count` objects.
    ///
    /// Static because the allocator carries no state that allocation could
    /// depend on. The standard's allocator requirements are written in terms of
    /// the expression `a.allocate(n)`, which a static member satisfies, so
    /// containers are unaffected by the distinction.
    [[nodiscard]] static T* allocate(std::size_t count) {
        if (count > max_size()) {
            // The multiplication below would wrap, and a wrapped byte count
            // allocates a small block that is then handed back as though it
            // were large. That is a silent buffer overflow rather than a failed
            // allocation, so the overflow is turned into the failure it is.
            throw std::bad_array_new_length{};
        }

        // The aligned form of operator new rather than a hand-rolled
        // over-allocate-and-offset scheme: it is the standard's own answer to
        // this question, it is available on all three supported compilers, and
        // it needs no bookkeeping to recover the original pointer.
        return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{kAlignment}));
    }

    static void deallocate(T* pointer, std::size_t count) noexcept {
        // The sized and aligned form, matching the allocation above. An
        // ordinary operator delete would be undefined behaviour here, since the
        // storage did not come from an ordinary operator new.
        ::operator delete(pointer, count * sizeof(T), std::align_val_t{kAlignment});
    }

    /// The largest count that can be expressed in bytes without wrapping.
    [[nodiscard]] static constexpr std::size_t max_size() noexcept {
        return std::numeric_limits<std::size_t>::max() / sizeof(T);
    }
};

/// All instances are interchangeable, whatever their value type, because the
/// allocator holds nothing that could distinguish them.
template<typename T, typename U>
[[nodiscard]] constexpr bool operator==(const AlignedAllocator<T>& /*lhs*/,
                                        const AlignedAllocator<U>& /*rhs*/) noexcept {
    return true;
}

} // namespace orrery::core
