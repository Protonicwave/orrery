/// \file
/// That each of this backend's allocations is the kind it claims to be, and
/// that the transfer between them is a transfer.
///
/// Phase 9 asked for the absence of a host-to-device copy to be demonstrated
/// rather than asserted, and `tests/backend/sycl_usm_test.cpp` demonstrated it:
/// a pointer the host wrote, a kernel read and modified through the same pointer
/// value, with no copy requested anywhere.
///
/// The demonstration here is the mirror image, and the point of it is that the
/// answer comes out the other way. On a discrete card the copy exists, it cannot
/// be argued away, and what has to be established instead is that the project is
/// paying for it deliberately: that the arrays the kernels read are device
/// memory, that the buffers the host assembles are pinned host memory, and that
/// neither is the managed allocation that would have made both questions
/// unanswerable.
///
/// The negative case matters as much as the positive ones. If a later change
/// replaced `cudaMalloc` with `cudaMallocManaged`, every figure this backend
/// produces would still look plausible while an unknown amount of transfer time
/// moved inside the kernel column. The third case below is what fails instead.

// Everything is inside the guard, including the includes, on the same terms as
// the SYCL tests: without the backend this file has no cases, and headers
// included ahead of a block that is compiled out are unused by construction,
// which the lint job reports as errors.

#ifdef ORRERY_ENABLE_CUDA

#    include "orrery/backend/cuda_memory.hpp"

#    include <cstddef>
#    include <memory>
#    include <utility>
#    include <vector>

#    include <catch2/catch_message.hpp>
#    include <catch2/catch_test_macros.hpp>

#    include "orrery/backend/cuda_device.hpp"

namespace {

using orrery::backend::CudaArray;
using orrery::backend::CudaHostArray;
using orrery::backend::CudaPointerKind;
using orrery::backend::discover_cuda_device;
using orrery::backend::pointer_kind;
using orrery::backend::to_string;

constexpr std::size_t kCount = 1024;

[[nodiscard]] bool has_device() {
    return discover_cuda_device().has_value();
}

} // namespace

TEST_CASE("Device memory is device memory and comes back unchanged", "[backend][cuda]") {
    if (!has_device()) {
        SKIP("no CUDA device on this machine");
    }

    CudaHostArray<float> staging{kCount};
    CudaArray<float> device{kCount};

    for (std::size_t i = 0; i < kCount; ++i) {
        staging.data()[i] = static_cast<float>(i);
    }

    device.copy_from_host(staging.data(), kCount);

    // Overwritten before the read back, so that a copy that quietly did nothing
    // cannot pass by leaving the original values in place. Without this the case
    // would assert that the host can remember what it wrote.
    for (std::size_t i = 0; i < kCount; ++i) {
        staging.data()[i] = -1.0F;
    }

    device.copy_to_host(staging.data(), kCount);

    for (std::size_t i = 0; i < kCount; ++i) {
        INFO("element " << i);
        REQUIRE(staging.data()[i] == static_cast<float>(i));
    }
}

TEST_CASE("The runtime agrees about what each allocation is", "[backend][cuda]") {
    if (!has_device()) {
        SKIP("no CUDA device on this machine");
    }

    const CudaArray<double> device{kCount};
    const CudaHostArray<double> pinned{kCount};

    INFO("device array reports " << to_string(pointer_kind(device.data())));
    INFO("staging array reports " << to_string(pointer_kind(pinned.data())));

    // The two answers this backend's whole timing story rests on. Device memory
    // is what the kernels read, which is why every evaluation has a transfer in
    // it; pinned host memory is what the transfer reads from, which is why the
    // transfer is measured rather than variable.
    REQUIRE(pointer_kind(device.data()) == CudaPointerKind::kDevice);
    REQUIRE(pointer_kind(pinned.data()) == CudaPointerKind::kPinnedHost);

    // Neither is managed, which is the allocation that would have hidden the
    // transfer inside the kernel time. Stated as its own requirement rather than
    // left implied by the two above, because it is the one a future change is
    // most likely to make by accident.
    REQUIRE(pointer_kind(device.data()) != CudaPointerKind::kManaged);
    REQUIRE(pointer_kind(pinned.data()) != CudaPointerKind::kManaged);
}

TEST_CASE("The pointer query discriminates rather than agreeing", "[backend][cuda]") {
    if (!has_device()) {
        SKIP("no CUDA device on this machine");
    }

    // A query that answered "device" to everything would satisfy the case above
    // while establishing nothing. Ordinary heap memory has to come back as
    // something else, and it is asked about after a real allocation so that the
    // runtime is initialised and the answer is the query's rather than a failure
    // to start.
    const std::vector<double> ordinary(kCount);
    const auto single = std::make_unique<double>(1.0);

    REQUIRE(pointer_kind(ordinary.data()) == CudaPointerKind::kUnknown);
    REQUIRE(pointer_kind(single.get()) == CudaPointerKind::kUnknown);

    // A null pointer is not an allocation of any kind and must not reach the
    // runtime, since asking about one records an error the next unrelated call
    // would report as its own.
    REQUIRE(pointer_kind(nullptr) == CudaPointerKind::kUnknown);
}

TEST_CASE("An empty allocation owns nothing and survives being moved", "[backend][cuda]") {
    if (!has_device()) {
        SKIP("no CUDA device on this machine");
    }

    // A configuration with no particles reaches the solvers by the same path as
    // any other, and `cudaMalloc` of zero bytes is not portable across runtimes,
    // so the zero case never reaches it.
    const CudaArray<float> empty_device{0};
    const CudaHostArray<float> empty_host{0};

    REQUIRE(empty_device.empty());
    REQUIRE(empty_device.data() == nullptr);
    REQUIRE(empty_host.empty());
    REQUIRE(empty_host.data() == nullptr);

    // Moving is how a solver grows its arrays between evaluations, so a move
    // that freed the wrong pointer would show up as a use-after-free at the next
    // timestep rather than here. The moved-from object must be safe to destroy.
    CudaArray<float> source{kCount};
    const float* const address = source.data();

    CudaArray<float> destination{std::move(source)};

    REQUIRE(destination.data() == address);
    REQUIRE(destination.size() == kCount);
    REQUIRE(source.data() == nullptr); // NOLINT(bugprone-use-after-move)
    REQUIRE(source.empty());           // NOLINT(bugprone-use-after-move)
}

#endif // ORRERY_ENABLE_CUDA
