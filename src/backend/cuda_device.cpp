#include "orrery/backend/cuda_device.hpp"

#include <optional>
#include <string>

#ifdef ORRERY_ENABLE_CUDA
#    include <cstdint>

#    include <cuda_runtime.h>
#endif

namespace orrery::backend {

bool CudaDeviceDescription::supports_build_precision() noexcept {
    // Every CUDA device implements both precisions, so both configurations run
    // and the question the SYCL description has to ask does not arise here. It
    // returns a constant rather than reading a field because there is no field
    // to read: the runtime reports no capability flag for double precision,
    // only a compute capability, and every capability the toolkit still
    // supports has it.
    return true;
}

std::string to_version_string(unsigned version) {
    // The runtime encodes 12.4 as 12040: major times a thousand plus minor times
    // ten. The last digit is a patch level the runtime does not populate.
    const unsigned major = version / 1000U;
    const unsigned minor = (version % 1000U) / 10U;
    return std::to_string(major) + "." + std::to_string(minor);
}

#ifdef ORRERY_ENABLE_CUDA

namespace {

/// Fill in a description from the properties the runtime reports.
///
/// Every field here is a query rather than a constant this project holds about
/// the part it was written for, on the same terms as the SYCL backend's
/// `describe`: the figures in `docs/performance/cuda.md` are taken on a machine
/// nobody working on this repository owns, so a description assembled from
/// assumptions would be a description of the wrong computer.
[[nodiscard]] CudaDeviceDescription describe(const cudaDeviceProp& properties, int driver,
                                             int runtime) {
    CudaDeviceDescription description;

    description.name = static_cast<const char*>(properties.name);

    description.compute_capability_major = static_cast<unsigned>(properties.major);
    description.compute_capability_minor = static_cast<unsigned>(properties.minor);
    description.multiprocessor_count = static_cast<unsigned>(properties.multiProcessorCount);
    description.warp_size = static_cast<unsigned>(properties.warpSize);
    description.max_threads_per_block = static_cast<unsigned>(properties.maxThreadsPerBlock);

    description.shared_memory_bytes_per_block =
        static_cast<std::uint64_t>(properties.sharedMemPerBlock);
    description.global_memory_bytes = static_cast<std::uint64_t>(properties.totalGlobalMem);

    description.integrated = properties.integrated != 0;
    description.unified_addressing = properties.unifiedAddressing != 0;
    description.supports_managed_memory = properties.managedMemory != 0;

    description.driver_version = static_cast<unsigned>(driver);
    description.runtime_version = static_cast<unsigned>(runtime);

    return description;
}

} // namespace

unsigned cuda_device_count() noexcept {
    int count = 0;

    // The runtime reports the absence of a driver, of a device and of a usable
    // device through the same return value, and this function's caller wants all
    // three collapsed into zero. The error is deliberately not cleared
    // afterwards: `cudaGetDeviceCount` is documented not to leave a sticky error
    // behind, and clearing state a query did not set would hide a real failure
    // that some earlier call had recorded.
    if (cudaGetDeviceCount(&count) != cudaSuccess || count < 0) {
        return 0;
    }

    return static_cast<unsigned>(count);
}

std::optional<CudaDeviceDescription> discover_cuda_device() noexcept {
    if (cuda_device_count() == 0) {
        return std::nullopt;
    }

    // The first device, for the reason the header gives: choosing between
    // several would need a policy, and the machines this backend is measured on
    // have one.
    cudaDeviceProp properties{};
    if (cudaGetDeviceProperties(&properties, 0) != cudaSuccess) {
        return std::nullopt;
    }

    // A driver older than the runtime the binary was linked against will
    // enumerate the device and then refuse every launch, which is the single
    // most common way a CUDA build fails on a machine that looks like it should
    // work. Both versions are reported so that the table says so, and neither is
    // treated as a reason to decline the device here: refusing to construct on a
    // version mismatch would turn a clear launch failure into a silent fallback
    // to the CPU.
    int driver = 0;
    int runtime = 0;
    if (cudaDriverGetVersion(&driver) != cudaSuccess ||
        cudaRuntimeGetVersion(&runtime) != cudaSuccess) {
        return std::nullopt;
    }

    return describe(properties, driver, runtime);
}

#else

// Without the backend there is no runtime to ask, and the answer to "is there a
// device to run on" is no. Returning empty rather than refusing to compile is
// what lets a benchmark or a test be written once and report that the backend
// was not built, rather than being conditionally compiled out of the program.

unsigned cuda_device_count() noexcept {
    return 0;
}

std::optional<CudaDeviceDescription> discover_cuda_device() noexcept {
    return std::nullopt;
}

#endif

std::string to_string(const CudaDeviceDescription& device) {
    std::string text = device.name;
    text += " (compute capability ";
    text += std::to_string(device.compute_capability_major);
    text += ".";
    text += std::to_string(device.compute_capability_minor);
    text += ", ";
    text += std::to_string(device.multiprocessor_count);
    text += " multiprocessors, warp ";
    text += std::to_string(device.warp_size);
    text += ", driver ";
    text += to_version_string(device.driver_version);
    text += ", runtime ";
    text += to_version_string(device.runtime_version);
    text += device.integrated ? ", integrated" : ", discrete";
    text += ")";
    return text;
}

} // namespace orrery::backend
