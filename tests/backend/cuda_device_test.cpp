#include "orrery/backend/cuda_device.hpp"

#include <optional>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

/// \file
/// That CUDA discovery answers, on every machine, without throwing.
///
/// The same property `tests/backend/sycl_device_test.cpp` asserts of the other
/// backend, and it matters more here rather than less. That backend's continuous
/// integration job runs on a machine with no Intel GPU; this one runs on a
/// machine with no NVIDIA GPU, no driver, and a toolkit installed purely so that
/// the device compiler has something to compile with. Every case below except
/// the last therefore runs in exactly the configuration the project's CUDA job
/// is in, which is the configuration a change that turned a missing device into
/// an error would break.
///
/// The cases split three ways for the same reason they do there: what must hold
/// without the backend, what must hold with the backend and no device, and what
/// must hold when a device is present. Only the last skips itself.

namespace {

using orrery::backend::cuda_device_count;
using orrery::backend::CudaDeviceDescription;
using orrery::backend::discover_cuda_device;
using orrery::backend::kCudaBackendCompiled;
using orrery::backend::to_string;
using orrery::backend::to_version_string;

} // namespace

TEST_CASE("CUDA discovery answers without throwing on any machine", "[backend][cuda]") {
    // The calls themselves are the assertion. Both are noexcept, so an escaping
    // exception would terminate rather than fail, and a machine with a driver
    // that enumerates a device it cannot use is precisely where one would come
    // from.
    const unsigned count = cuda_device_count();
    const std::optional<CudaDeviceDescription> device = discover_cuda_device();

    if constexpr (!kCudaBackendCompiled) {
        // Without the backend there is no runtime to ask, and both must say so
        // rather than reporting a device the build could not launch on.
        REQUIRE(count == 0);
        REQUIRE_FALSE(device.has_value());
    } else {
        // With the backend, describing a device implies there was one to
        // describe. The converse does not hold: a runtime that counts a device
        // and then fails to report its properties is a broken driver, which is a
        // state this layer answers "no device" to rather than propagating.
        if (device.has_value()) {
            REQUIRE(count > 0);
        }
        SUCCEED("discovery completed");
    }
}

TEST_CASE("A CUDA version is written the way people say it", "[backend][cuda]") {
    // The runtime packs 12.4 into 12040, and every caller that prints a version
    // would otherwise repeat the arithmetic. This is the one part of the
    // discovery layer that can be checked on a machine with no device at all,
    // which is why it is checked rather than left to a device that may never
    // arrive.
    REQUIRE(to_version_string(12040) == "12.4");
    REQUIRE(to_version_string(11080) == "11.8");
    REQUIRE(to_version_string(13000) == "13.0");

    // Zero is what an unpopulated field holds, and it must format rather than
    // divide by anything.
    REQUIRE(to_version_string(0) == "0.0");
}

TEST_CASE("A discovered CUDA device describes itself consistently", "[backend][cuda]") {
    const std::optional<CudaDeviceDescription> device = discover_cuda_device();
    if (!device.has_value()) {
        SKIP("no CUDA device on this machine, or this build has no CUDA backend");
    }

    INFO("device: " << to_string(*device));

    REQUIRE_FALSE(device->name.empty());

    // A device reporting none of these would be one no kernel could be launched
    // on, so a zero here means the query failed rather than that the hardware is
    // unusual.
    REQUIRE(device->compute_capability_major > 0);
    REQUIRE(device->multiprocessor_count > 0);
    REQUIRE(device->max_threads_per_block > 0);
    REQUIRE(device->global_memory_bytes > 0);
    REQUIRE(device->shared_memory_bytes_per_block > 0);

    // The width the coherent traversal is built around. Every NVIDIA part
    // shipped so far reports 32, and the traversal reads the field rather than
    // assuming it, so what is asserted here is that the field was populated.
    REQUIRE(device->warp_size > 0);

    // Both precisions run on every CUDA device, so this predicate is a constant
    // and says so. It is asserted rather than omitted because a caller written
    // against both backends asks it of each, and a version of this that started
    // returning false would send every double-precision run to the CPU without
    // any other test noticing.
    REQUIRE(device->supports_build_precision());

    // A driver older than the runtime enumerates the device and then refuses
    // every launch, which is the most common way a CUDA build fails on a machine
    // that looks like it should work. Discovery deliberately does not decline
    // such a device, so the mismatch is recorded here where a failing suite can
    // be read rather than diagnosed.
    INFO("driver " << to_version_string(device->driver_version) << ", runtime "
                   << to_version_string(device->runtime_version));
    REQUIRE(device->driver_version > 0);
    REQUIRE(device->runtime_version > 0);

    const std::string summary = to_string(*device);
    REQUIRE(summary.find(device->name) != std::string::npos);
    REQUIRE(summary.find(device->integrated ? "integrated" : "discrete") != std::string::npos);
}
