#include "orrery/backend/sycl_device.hpp"

#include <optional>
#include <string>

#include "orrery/core/types.hpp"

#ifdef ORRERY_ENABLE_SYCL
#include <algorithm>
#include <cstdint>
#include <vector>

#include <sycl/sycl.hpp>
#endif

namespace orrery::backend {

bool DeviceDescription::supports_build_precision() const noexcept {
    // A single-precision build asks nothing of the device that any GPU fails to
    // provide. A double-precision build asks for fp64, which is the question
    // that decides whether this project's accuracy-oriented configuration can
    // run on the GPU at all.
    return core::kSinglePrecision || supports_double_precision;
}

#ifdef ORRERY_ENABLE_SYCL

namespace {

/// Fill in a description from a device the runtime handed us.
///
/// Every `get_info` call here is a query against the driver rather than a
/// constant this project holds about the part it was written for. That is the
/// point: section 2 of the implementation plan records nominal figures for the
/// target machine, and Phase 7 established that the project quotes measured
/// values rather than nominal ones wherever it can.
[[nodiscard]] DeviceDescription describe(const sycl::device& device) {
    DeviceDescription description;

    description.name = device.get_info<sycl::info::device::name>();
    description.vendor = device.get_info<sycl::info::device::vendor>();
    description.driver_version = device.get_info<sycl::info::device::driver_version>();
    description.backend = device.get_platform().get_info<sycl::info::platform::name>();

    description.compute_units = device.get_info<sycl::info::device::max_compute_units>();
    description.max_work_group_size =
        static_cast<unsigned>(device.get_info<sycl::info::device::max_work_group_size>());

    // The widest sub-group the device offers, which on Intel Xe is the widest
    // SIMD the hardware will execute a kernel at. Reported rather than chosen
    // here: which width the kernel actually requests is the solver's decision
    // and is recorded beside its figures.
    const std::vector<std::size_t> widths = device.get_info<sycl::info::device::sub_group_sizes>();
    if (!widths.empty()) {
        description.sub_group_size =
            static_cast<unsigned>(*std::max_element(widths.begin(), widths.end()));
    }

    description.global_memory_bytes =
        static_cast<std::uint64_t>(device.get_info<sycl::info::device::global_mem_size>());
    description.local_memory_bytes =
        static_cast<std::uint64_t>(device.get_info<sycl::info::device::local_mem_size>());

    description.supports_shared_usm = device.has(sycl::aspect::usm_shared_allocations);
    description.supports_device_usm = device.has(sycl::aspect::usm_device_allocations);
    description.supports_host_usm = device.has(sycl::aspect::usm_host_allocations);
    description.supports_system_usm = device.has(sycl::aspect::usm_system_allocations);
    description.supports_double_precision = device.has(sycl::aspect::fp64);

    return description;
}

} // namespace

std::optional<DeviceDescription> discover_gpu_device() noexcept {
    // The whole body is guarded. `sycl::device::get_devices` throws when the
    // runtime is present but cannot initialise, which is the state a machine is
    // left in by a partially installed or mismatched graphics driver, and it is
    // exactly the case this function exists to answer "no device" to rather than
    // to propagate. The function is noexcept, so an escaping exception would
    // terminate the process during what is meant to be a capability query.
    try {
        const std::vector<sycl::device> devices =
            sycl::device::get_devices(sycl::info::device_type::gpu);
        if (devices.empty()) {
            return std::nullopt;
        }

        // The first GPU, which on a machine with one integrated part is the
        // only one. A machine with several would want a selector, and this
        // project has no way to prefer between devices it has never measured.
        return describe(devices.front());
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<DeviceDescription> describe_default_device() noexcept {
    try {
        return describe(sycl::device{sycl::default_selector_v});
    } catch (...) {
        return std::nullopt;
    }
}

#else

// Without the backend there is no runtime to ask, and the answer to "is there a
// GPU to run on" is no. Returning empty rather than refusing to compile is what
// lets a benchmark or a test be written once and report "not built with SYCL"
// on an ordinary build instead of being conditionally compiled out of it.

std::optional<DeviceDescription> discover_gpu_device() noexcept { return std::nullopt; }

std::optional<DeviceDescription> describe_default_device() noexcept { return std::nullopt; }

#endif

std::string to_string(const DeviceDescription& device) {
    std::string text = device.name;
    text += " (";
    text += device.backend;
    text += ", driver ";
    text += device.driver_version;
    text += ", ";
    text += std::to_string(device.compute_units);
    text += " compute units, ";
    text += device.supports_shared_usm ? "shared USM" : "no shared USM";
    if (device.supports_system_usm) {
        text += ", system USM";
    }
    if (!device.supports_double_precision) {
        text += ", no fp64";
    }
    text += ")";
    return text;
}

} // namespace orrery::backend
