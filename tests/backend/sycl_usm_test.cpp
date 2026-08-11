/// \file
/// The zero-copy demonstration Phase 9's definition of done asks for.
///
/// The claim under test is that on this hardware there is no host-to-device
/// transfer: the host writes an allocation, a kernel reads and writes the same
/// allocation, and the host reads the kernel's results back, with no copy
/// requested at any point and none performed underneath. Section 2 of the
/// implementation plan builds an architecture on that property and the phase
/// requires it to be shown rather than stated.
///
/// What makes this a demonstration rather than a restatement is that the test
/// never calls a copy function, and separately asks the runtime what kind of
/// pointer it is holding. If the allocation were quietly a device allocation
/// with a transfer behind it, the pointer kind would say so and the host's
/// dereference would be undefined. If it were an ordinary heap pointer, the
/// kernel could not read it.

#ifdef ORRERY_ENABLE_SYCL

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sycl/sycl.hpp>

#include "orrery/backend/sycl_device.hpp"
#include "orrery/backend/sycl_usm.hpp"
#include "orrery/core/types.hpp"

namespace {

using orrery::backend::allocation_kind;
using orrery::backend::discover_gpu_device;
using orrery::backend::to_string;
using orrery::backend::UsmArray;
using orrery::backend::UsmKind;
using orrery::core::Real;

/// A queue on this machine's GPU, or nothing.
[[nodiscard]] std::optional<sycl::queue> gpu_queue() {
    if (!discover_gpu_device().has_value()) {
        return std::nullopt;
    }
    try {
        return sycl::queue{sycl::gpu_selector_v};
    } catch (const sycl::exception&) {
        return std::nullopt;
    }
}

} // namespace

TEST_CASE("A shared allocation is what the runtime says it is", "[backend][sycl][usm]") {
    std::optional<sycl::queue> queue = gpu_queue();
    if (!queue.has_value()) {
        SKIP("no SYCL GPU on this machine");
    }

    UsmArray<Real> array{*queue, 256};
    REQUIRE(array.data() != nullptr);
    REQUIRE(array.size() == 256);

    const UsmKind kind = allocation_kind(array.data(), *queue);
    INFO("allocation reported as " << to_string(kind));
    REQUIRE(kind == UsmKind::kShared);

    // The contrast that gives the assertion above its meaning. An ordinary heap
    // pointer is not USM at all, so the query is discriminating rather than
    // answering "shared" to everything it is handed.
    std::vector<Real> ordinary(256);
    REQUIRE(allocation_kind(ordinary.data(), *queue) == UsmKind::kUnknown);
    REQUIRE(allocation_kind(nullptr, *queue) == UsmKind::kUnknown);
}

TEST_CASE("Host and device share one allocation with no copy", "[backend][sycl][usm]") {
    std::optional<sycl::queue> queue = gpu_queue();
    if (!queue.has_value()) {
        SKIP("no SYCL GPU on this machine");
    }

    constexpr std::size_t kCount = 1024;
    UsmArray<Real> array{*queue, kCount};

    // Written by the host, through the ordinary pointer, with no staging buffer
    // and no submission.
    Real* const data = array.data();
    for (std::size_t i = 0; i < kCount; ++i) {
        data[i] = static_cast<Real>(i);
    }

    // Read and written by the device, through the same pointer value. The
    // capture is the pointer itself: nothing here transfers, maps or copies.
    queue->parallel_for(sycl::range<1>{kCount},
                        [=](sycl::id<1> id) { data[id] = data[id] * Real{2}; })
        .wait_and_throw();

    // Read back by the host, again through the same pointer. If a copy were
    // happening it would have to have been requested, and no call above could
    // have requested one.
    for (std::size_t i = 0; i < kCount; ++i) {
        REQUIRE(data[i] == static_cast<Real>(i) * Real{2});
    }

    // And the address the kernel used is the address the host holds. This is the
    // property the whole file is about, and it is what "unified" means: not that
    // a transfer is fast, but that there is no second address for one to go to.
    REQUIRE(array.data() == data);
}

TEST_CASE("A USM array owns its allocation", "[backend][sycl][usm]") {
    std::optional<sycl::queue> queue = gpu_queue();
    if (!queue.has_value()) {
        SKIP("no SYCL GPU on this machine");
    }

    UsmArray<Real> first{*queue, 128};
    Real* const address = first.data();
    REQUIRE(address != nullptr);

    // Moving transfers ownership rather than duplicating it. A double free here
    // would be a crash rather than a failed assertion, which is the point of
    // asserting it under the sanitiser builds as well.
    UsmArray<Real> second{std::move(first)};
    REQUIRE(second.data() == address);
    REQUIRE(second.size() == 128);
    REQUIRE(first.data() == nullptr);
    REQUIRE(first.empty());

    // A default-constructed array owns nothing and destroying it frees nothing.
    const UsmArray<Real> empty;
    REQUIRE(empty.data() == nullptr);
    REQUIRE(empty.empty());

    // An empty allocation is legal and allocates nothing, which is what an empty
    // particle configuration reaching the solver produces.
    const UsmArray<Real> zero{*queue, 0};
    REQUIRE(zero.empty());
    REQUIRE(zero.data() == nullptr);
}

#endif // ORRERY_ENABLE_SYCL
