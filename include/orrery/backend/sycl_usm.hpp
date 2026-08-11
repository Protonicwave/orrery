#pragma once

/// \file
/// Unified shared memory, owned properly, and the evidence that it is unified.
///
/// On the target hardware the GPU is on the same package as the CPU and reads
/// the same physical memory (section 2 of the implementation plan). SYCL's
/// unified shared memory is how a program says so: an allocation made this way
/// has one address that is valid in host code and in device code, and passing it
/// to a kernel passes a pointer rather than scheduling a transfer.
///
/// ## Which of the three kinds, and why it matters
///
/// USM offers three allocation kinds and only one of them is right here.
///
/// Device allocations live in device memory and are not addressable from the
/// host, so filling one means an explicit copy. On a discrete card that is the
/// correct choice and the copy is the price of the fastest memory on the board.
/// Here there is no separate memory for it to live in, so the copy would be from
/// system memory to system memory: pure loss.
///
/// Host allocations live in host memory and are addressable from the device,
/// which sounds right but is not. On a discrete system they are read across the
/// bus on demand. The kind is meant for data touched once, and the runtime is
/// entitled to treat it as uncached from the device's point of view.
///
/// Shared allocations are addressable from both and the runtime is free to place
/// the pages wherever it likes, including nowhere in particular when host and
/// device share a memory controller. That is the kind this backend uses, and on
/// a unified part it is the kind for which no copy exists to elide.
///
/// ## Why the kind is queried rather than assumed
///
/// Phase 9's definition of done asks for the absence of a host-to-device copy to
/// be demonstrated rather than asserted, and "I called the shared allocator" is
/// an assertion. `allocation_kind` asks the runtime what a pointer actually is,
/// through the same query any tool would use, and `tests/backend/sycl_usm_test.cpp`
/// checks that a pointer the host wrote is reported as shared, is readable from a
/// kernel, and comes back with the kernel's changes visible on the host without
/// any copy having been requested. That is a test rather than a paragraph, which
/// is the distinction the phase is asking for.

#ifdef ORRERY_ENABLE_SYCL

#    include <cstddef>
#    include <cstdint>
#    include <new>
#    include <span>
#    include <utility>

#    include <sycl/sycl.hpp>

namespace orrery::backend {

/// Which kind of USM allocation a pointer is, as the runtime sees it.
///
/// Mirrors `sycl::usm::alloc` in a type that does not require the caller to
/// include a device runtime header, so that a test message or a report can name
/// the kind without the file that prints it becoming a SYCL translation unit.
enum class UsmKind : std::uint8_t {
    /// Not a USM pointer at all. What an ordinary `new` returns, and what a
    /// mistake looks like.
    kUnknown,

    /// Host memory, device-addressable.
    kHost,

    /// Device memory, not host-addressable.
    kDevice,

    /// Addressable from both. What this backend allocates.
    kShared,
};

[[nodiscard]] constexpr const char* to_string(UsmKind kind) noexcept {
    switch (kind) {
    case UsmKind::kHost:
        return "host";
    case UsmKind::kDevice:
        return "device";
    case UsmKind::kShared:
        return "shared";
    case UsmKind::kUnknown:
        break;
    }
    return "unknown";
}

/// What the runtime reports `pointer` to be, in `queue`'s context.
///
/// The demonstration described above. A pointer that this backend allocated
/// answers `kShared`; one that came from `new` answers `kUnknown`.
[[nodiscard]] UsmKind allocation_kind(const void* pointer, const sycl::queue& queue) noexcept;

/// An owning, movable, cache-line-aligned shared USM allocation.
///
/// The project forbids owning raw pointers (section 4), and `sycl::malloc_shared`
/// hands back exactly that, paired with a `sycl::free` that needs the same queue
/// the allocation was made against. This is the RAII wrapper that makes the pair
/// impossible to separate.
///
/// Aligned to a cache line for the reason `core/aligned_allocator.hpp` gives for
/// the host arrays: the kernel reads these as contiguous component arrays, and an
/// allocation that starts mid-line makes every vector load of the first elements
/// straddle two lines. ADR-0005 sets out the argument, which does not stop being
/// true because the reader is a GPU.
template<typename T> class UsmArray {
public:
    /// An empty array, owning nothing.
    UsmArray() noexcept = default;

    /// `count` default-initialised elements of shared USM from `queue`'s device.
    ///
    /// Throws `std::bad_alloc` when the runtime cannot satisfy the request,
    /// which is a setup-boundary failure of the kind section 4 permits
    /// exceptions at. No kernel is on the stack at this point.
    UsmArray(sycl::queue& queue, std::size_t count)
        : queue_(&queue),
          size_(count),
          data_(count == 0 ? nullptr
                           : sycl::aligned_alloc_shared<T>(kAlignmentBytes, count, queue)) {
        if (count != 0 && data_ == nullptr) {
            throw std::bad_alloc{};
        }
    }

    ~UsmArray() { release(); }

    UsmArray(const UsmArray&) = delete;
    UsmArray& operator=(const UsmArray&) = delete;

    UsmArray(UsmArray&& other) noexcept
        : queue_(std::exchange(other.queue_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          data_(std::exchange(other.data_, nullptr)) {}

    UsmArray& operator=(UsmArray&& other) noexcept {
        if (this != &other) {
            release();
            queue_ = std::exchange(other.queue_, nullptr);
            size_ = std::exchange(other.size_, 0);
            data_ = std::exchange(other.data_, nullptr);
        }
        return *this;
    }

    /// The pointer, valid in host code and in device code alike.
    ///
    /// This is the property the whole file is about. The value returned here is
    /// what a kernel captures, and it is the same number the host dereferences.
    [[nodiscard]] T* data() const noexcept { return data_; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] std::span<T> span() const noexcept { return {data_, size_}; }

private:
    /// A cache line on the target CPU, and the granularity the GPU's memory
    /// controller shares with it.
    static constexpr std::size_t kAlignmentBytes = 64;

    void release() noexcept {
        if (data_ != nullptr && queue_ != nullptr) {
            sycl::free(data_, *queue_);
        }
        data_ = nullptr;
        size_ = 0;
        queue_ = nullptr;
    }

    /// Not owned. The queue must outlive the allocation, which it does because
    /// the solver holds both and destroys them in reverse order of declaration.
    sycl::queue* queue_{nullptr};
    std::size_t size_{0};
    T* data_{nullptr};
};

} // namespace orrery::backend

#endif // ORRERY_ENABLE_SYCL
