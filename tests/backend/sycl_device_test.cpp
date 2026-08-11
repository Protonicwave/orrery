#include "orrery/backend/sycl_device.hpp"

#include <optional>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/types.hpp"

/// \file
/// That device discovery answers, on every machine, without throwing.
///
/// The interesting property here is not what the answer is. It is that there is
/// an answer at all on a machine with no GPU, no driver and no SYCL runtime,
/// because a continuous integration runner is exactly such a machine and this is
/// the layer that has to degrade rather than fail there. Phase 9's deliverables
/// call it graceful fallback, and this is where the grace is asserted.
///
/// The cases below therefore split three ways: what must hold in a build without
/// the backend, what must hold in a build with the backend but no device, and
/// what must hold when a device is actually present. Only the last is skipped on
/// a machine that cannot run it.

namespace {

using orrery::backend::describe_default_device;
using orrery::backend::DeviceDescription;
using orrery::backend::discover_gpu_device;
using orrery::backend::kSyclBackendCompiled;
using orrery::backend::to_string;
using orrery::core::kSinglePrecision;

} // namespace

TEST_CASE("Discovery answers without throwing on any machine", "[backend][sycl]") {
    // The call itself is the assertion. Both are noexcept, so an escaping
    // exception would terminate rather than fail, and a machine with a broken
    // driver is precisely where one would come from.
    const std::optional<DeviceDescription> gpu = discover_gpu_device();
    const std::optional<DeviceDescription> any = describe_default_device();

    if constexpr (!kSyclBackendCompiled) {
        // Without the backend there is no runtime to ask, and both must say so
        // rather than reporting a device the build cannot use.
        REQUIRE_FALSE(gpu.has_value());
        REQUIRE_FALSE(any.has_value());
    } else {
        // With the backend, finding a GPU implies finding some device. The
        // converse does not hold: a machine with only a CPU device is ordinary.
        if (gpu.has_value()) {
            REQUIRE(any.has_value());
        }
        SUCCEED("discovery completed");
    }
}

TEST_CASE("A discovered GPU describes itself consistently", "[backend][sycl]") {
    const std::optional<DeviceDescription> gpu = discover_gpu_device();
    if (!gpu.has_value()) {
        SKIP("no SYCL GPU on this machine, or this build has no SYCL backend");
    }

    INFO("device: " << to_string(*gpu));

    REQUIRE_FALSE(gpu->name.empty());
    REQUIRE_FALSE(gpu->driver_version.empty());
    REQUIRE_FALSE(gpu->backend.empty());

    // A device that reported none of these would be one no kernel could be
    // launched on, so a zero here means the query failed rather than that the
    // hardware is unusual.
    REQUIRE(gpu->compute_units > 0);
    REQUIRE(gpu->max_work_group_size > 0);
    REQUIRE(gpu->global_memory_bytes > 0);

    // The backend allocates shared USM and has no fallback that does not. A GPU
    // without it is one `SyclDirectSolver::try_create` declines, and this
    // records which of the two situations a failure to construct came from.
    REQUIRE(gpu->supports_shared_usm);

    // The precision predicate has to agree with the aspect it is derived from.
    // In a single-precision build the answer is unconditional; in a
    // double-precision build it is exactly the fp64 aspect.
    if constexpr (kSinglePrecision) {
        REQUIRE(gpu->supports_build_precision());
    } else {
        REQUIRE(gpu->supports_build_precision() == gpu->supports_double_precision);
    }

    const std::string summary = to_string(*gpu);
    REQUIRE(summary.find(gpu->name) != std::string::npos);
    REQUIRE(summary.find(gpu->driver_version) != std::string::npos);
}
