#pragma once

/// \file
/// Device memory and pinned host memory, owned properly, and the evidence that
/// each is what it claims to be.
///
/// The counterpart of `backend/sycl_usm.hpp`, and the place where the two GPU
/// backends of this project stop resembling each other. That file exists to
/// explain why one allocation kind is right on a part where the CPU and the GPU
/// read the same physical memory. This one exists because on a discrete card
/// they do not, and the copy that Phase 9 was able to argue away has to be paid
/// for here instead.
///
/// ## Which allocation, and why not the convenient one
///
/// CUDA offers the same three kinds SYCL does under different names, and a
/// fourth that looks like the obvious answer.
///
/// `cudaMallocManaged` gives one pointer valid on both sides, and the runtime
/// moves pages between host and device as they are touched. It is the nearest
/// thing to the shared unified memory Phase 9 used, and porting the SYCL solver
/// to it would have been a two-line change. It is not what this backend
/// allocates, and ADR-0060 records why: on a discrete card managed memory does
/// not remove the transfer, it performs the same transfer at a moment nobody
/// chose, one page fault at a time, and charges it to whichever kernel happened
/// to touch the page first. A solver built on it would report a kernel time that
/// silently included an unknown amount of copying, which is precisely the
/// confusion `SyclEvaluationTimings` was introduced to prevent. The transfer is
/// real here, so it is made explicit, timed, and reported in its own column.
///
/// `cudaMalloc` gives memory only the device can address. That is what the
/// kernels read and write.
///
/// `cudaHostAlloc` gives host memory the driver has pinned, meaning the
/// operating system may not page it out, which is what allows the copy engine to
/// read it by direct memory access without the driver staging it through a
/// bounce buffer first. This is what the solver assembles its component arrays
/// into before sending them. It is not an optimisation added late: an unpinned
/// copy is roughly half the speed and, more importantly, its cost varies with
/// what else the machine is doing, which would put a term the benchmark cannot
/// control inside a figure the benchmark is quoting.
///
/// ## Why the kind is queried rather than asserted
///
/// Phase 9 asked for the absence of a host-to-device copy to be demonstrated
/// rather than asserted, and answered with `allocation_kind`. The same question
/// is worth asking here and the expected answer is the opposite one, which is
/// the point. `pointer_kind` asks the runtime what a pointer actually is, and
/// `tests/backend/cuda_memory_test.cpp` requires the solver's device arrays to
/// answer `device` and its staging buffers to answer `pinned host`.
///
/// That test is not a formality. It is what stops this backend quietly becoming
/// a managed-memory backend during some later change, which would keep every
/// figure looking plausible while moving an unknown amount of transfer time
/// inside the kernel column.

#ifdef ORRERY_ENABLE_CUDA

#    include <cstddef>
#    include <cstdint>
#    include <new>
#    include <span>
#    include <stdexcept>
#    include <string>
#    include <utility>

#    include <cuda_runtime.h>

namespace orrery::backend {

/// Which kind of allocation a pointer is, as the runtime sees it.
///
/// Mirrors what `cudaPointerGetAttributes` reports, in a type that does not
/// require the caller to include the toolkit's headers, so that a test message
/// or a benchmark table can name the kind without becoming a CUDA translation
/// unit. The same service `UsmKind` performs for the other backend.
enum class CudaPointerKind : std::uint8_t {
    /// Not known to the runtime at all. What an ordinary `new` returns, and
    /// therefore what a staging buffer this backend forgot to pin would answer.
    kUnknown,

    /// Host memory the driver has pinned or registered, which is the only kind
    /// of host memory the runtime knows anything about. What this backend stages
    /// through.
    kPinnedHost,

    /// Device memory. What the kernels read.
    kDevice,

    /// Migrated on demand between the two. Deliberately not used here, for the
    /// reason the file comment gives.
    kManaged,
};

[[nodiscard]] constexpr const char* to_string(CudaPointerKind kind) noexcept {
    switch (kind) {
    case CudaPointerKind::kPinnedHost:
        return "pinned host";
    case CudaPointerKind::kDevice:
        return "device";
    case CudaPointerKind::kManaged:
        return "managed";
    case CudaPointerKind::kUnknown:
        break;
    }
    return "unknown";
}

/// What the runtime reports `pointer` to be.
///
/// The demonstration described above. A pointer from `CudaArray` answers
/// `kDevice`, one from `CudaHostArray` answers `kPinnedHost`, and one from `new`
/// answers `kUnknown`.
///
/// Clears the runtime's error state before returning, because asking about a
/// pointer the runtime does not recognise is how this function answers
/// `kUnknown` and CUDA records that as an error the next unrelated call would
/// otherwise report.
[[nodiscard]] CudaPointerKind pointer_kind(const void* pointer) noexcept;

/// A CUDA runtime call that failed, with what it was doing.
///
/// The runtime reports errors as return values, which section 4 of the
/// implementation plan prefers at a boundary. What it does not offer is any way
/// for a caller three layers up to act on one: a failed allocation or a failed
/// launch means this machine cannot run the kernel, and every caller's response
/// is the same. So the boundary converts them, once, at the points where the
/// project already permits exceptions, and this is the type it converts them to.
class CudaError : public std::runtime_error {
public:
    CudaError(cudaError_t status, const char* operation)
        : std::runtime_error(std::string{operation} + " failed: " + cudaGetErrorString(status)),
          status_(status) {}

    /// The runtime's own code, for a caller that wants to distinguish one
    /// failure from another. Nothing in this project does, and it is kept
    /// because a diagnostic that has discarded the error code cannot be improved
    /// later without changing the type.
    [[nodiscard]] cudaError_t status() const noexcept { return status_; }

private:
    cudaError_t status_;
};

/// Throw unless `status` is success.
///
/// Used at construction, at submission and after synchronisation, and nowhere
/// inside a loop over particles.
inline void check_cuda(cudaError_t status, const char* operation) {
    if (status != cudaSuccess) {
        throw CudaError{status, operation};
    }
}

/// An owning, movable device allocation.
///
/// The project forbids owning raw pointers (section 4), and `cudaMalloc` hands
/// back exactly that paired with a `cudaFree` that must not be forgotten. This
/// is the wrapper that makes the pair impossible to separate, and it is the
/// direct analogue of `UsmArray` with one difference that matters: the pointer
/// it holds is not dereferenceable from the host, so it offers no `span` and no
/// `operator[]`. Only `copy_from_host` and `copy_to_host` move data across, and
/// a mistake that would have been a silent wrong answer on shared memory is a
/// compile error here.
///
/// No alignment is requested. `cudaMalloc` is documented to return memory
/// suitably aligned for any type and in practice returns 256-byte alignment,
/// which is already the granularity the memory controller wants; asking for more
/// would be asking for something the allocator has no way to give less than.
template<typename T> class CudaArray {
public:
    /// An empty array, owning nothing.
    CudaArray() noexcept = default;

    /// `count` elements of device memory, uninitialised.
    ///
    /// Throws `CudaError` when the runtime cannot satisfy the request, which is
    /// a setup-boundary failure of the kind section 4 permits exceptions at. No
    /// kernel is in flight at this point.
    ///
    /// A discrete card can genuinely run out, which the integrated part of Phase
    /// 9 could not in any configuration this project runs: there the allocation
    /// came from the machine's 32 GB, and here it comes from the card's own
    /// memory, which on the measured device is 16 GB and on smaller cards much
    /// less. That is a real difference in failure mode and it is the reason this
    /// constructor reports rather than asserts.
    explicit CudaArray(std::size_t count) : size_(count) {
        if (count == 0) {
            return;
        }

        void* pointer = nullptr;
        check_cuda(cudaMalloc(&pointer, count * sizeof(T)), "cudaMalloc");
        data_ = static_cast<T*>(pointer);
    }

    ~CudaArray() { release(); }

    CudaArray(const CudaArray&) = delete;
    CudaArray& operator=(const CudaArray&) = delete;

    CudaArray(CudaArray&& other) noexcept
        : size_(std::exchange(other.size_, 0)), data_(std::exchange(other.data_, nullptr)) {}

    CudaArray& operator=(CudaArray&& other) noexcept {
        if (this != &other) {
            release();
            size_ = std::exchange(other.size_, 0);
            data_ = std::exchange(other.data_, nullptr);
        }
        return *this;
    }

    /// The device pointer, which is what a kernel launch is given and what the
    /// host must not dereference.
    [[nodiscard]] T* data() const noexcept { return data_; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    /// Copy `source` into the front of this allocation, and wait for it.
    ///
    /// Synchronous, because the caller is a force evaluation with nothing to
    /// overlap the wait with: the kernel cannot start until the sources have
    /// arrived and the integrator cannot advance until the kernel has finished.
    /// An asynchronous copy would be the right shape for a solver that
    /// pipelined timesteps, which this one has no way to do, and it would make
    /// the timing breakdown report when a copy was issued rather than when it
    /// completed.
    void copy_from_host(const T* source, std::size_t count) {
        if (count == 0) {
            return;
        }
        check_cuda(cudaMemcpy(data_, source, count * sizeof(T), cudaMemcpyHostToDevice),
                   "cudaMemcpy to the device");
    }

    void copy_to_host(T* destination, std::size_t count) const {
        if (count == 0) {
            return;
        }
        check_cuda(cudaMemcpy(destination, data_, count * sizeof(T), cudaMemcpyDeviceToHost),
                   "cudaMemcpy from the device");
    }

    /// Set `count` elements to zero on the device.
    ///
    /// The counterpart of the `std::fill` the SYCL solvers run over their padded
    /// tail. There the tail was host memory and filling it cost a store per
    /// element on the CPU; here it is device memory, so it is filled by the
    /// device rather than filled on the host and sent.
    void fill_zero(std::size_t count) {
        if (count == 0) {
            return;
        }
        check_cuda(cudaMemset(data_, 0, count * sizeof(T)), "cudaMemset");
    }

private:
    void release() noexcept {
        if (data_ != nullptr) {
            // Nothing useful can be done with a failure here. It happens when
            // the context has already been destroyed, which is the ordinary
            // state during static destruction after a driver reset, and throwing
            // from a destructor would turn a device that disappeared into a
            // terminated process.
            static_cast<void>(cudaFree(data_));
        }
        data_ = nullptr;
        size_ = 0;
    }

    std::size_t size_{0};
    T* data_{nullptr};
};

/// An owning, movable pinned host allocation.
///
/// Host memory the host may read and write like any other, which the driver has
/// undertaken not to page out. The solver assembles its component arrays here
/// and sends the whole block in one transfer.
///
/// Pinned memory is a system-wide resource rather than a per-process one: pin
/// too much of a machine's RAM and every other process slows down. The
/// allocations this backend makes are a few hundred megabytes at the largest
/// configuration it runs, on a machine dedicated to the run, which is well
/// inside what the practice tolerates. A solver that pinned an arbitrary
/// fraction of a shared machine's memory would not be.
template<typename T> class CudaHostArray {
public:
    CudaHostArray() noexcept = default;

    /// `count` elements of pinned host memory, uninitialised.
    ///
    /// Throws `CudaError` on failure, and `std::bad_alloc` is not used even
    /// though this is an allocation: what fails here is a driver call, the
    /// runtime has a reason for the failure, and discarding it in favour of the
    /// standard exception would discard the only diagnostic there is.
    explicit CudaHostArray(std::size_t count) : size_(count) {
        if (count == 0) {
            return;
        }

        void* pointer = nullptr;
        check_cuda(cudaHostAlloc(&pointer, count * sizeof(T), cudaHostAllocDefault),
                   "cudaHostAlloc");
        data_ = static_cast<T*>(pointer);
    }

    ~CudaHostArray() { release(); }

    CudaHostArray(const CudaHostArray&) = delete;
    CudaHostArray& operator=(const CudaHostArray&) = delete;

    CudaHostArray(CudaHostArray&& other) noexcept
        : size_(std::exchange(other.size_, 0)), data_(std::exchange(other.data_, nullptr)) {}

    CudaHostArray& operator=(CudaHostArray&& other) noexcept {
        if (this != &other) {
            release();
            size_ = std::exchange(other.size_, 0);
            data_ = std::exchange(other.data_, nullptr);
        }
        return *this;
    }

    /// The pointer, which the host may dereference.
    [[nodiscard]] T* data() const noexcept { return data_; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] std::span<T> span() const noexcept { return {data_, size_}; }

private:
    void release() noexcept {
        if (data_ != nullptr) {
            static_cast<void>(cudaFreeHost(data_));
        }
        data_ = nullptr;
        size_ = 0;
    }

    std::size_t size_{0};
    T* data_{nullptr};
};

} // namespace orrery::backend

#endif // ORRERY_ENABLE_CUDA
