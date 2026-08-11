#pragma once

/// \file
/// Finding the GPU, and reporting what was found.
///
/// This is the only part of the SYCL backend that a caller compiled without
/// SYCL can still ask questions of. Everything else in the backend is guarded on
/// `ORRERY_ENABLE_SYCL` and simply does not exist in an ordinary build, which is
/// the same arrangement the AVX2 kernel uses and for the same reason: the
/// question of whether a code path was compiled is the one question C++ cannot
/// ask about itself.
///
/// ## Why discovery is a query rather than a constructor
///
/// There are three distinct ways a machine can fail to run a GPU kernel, and a
/// caller usually wants to tell them apart. The build may not include the
/// backend at all. The build may include it while the machine has no device the
/// runtime recognises, which is the ordinary state of a continuous integration
/// runner. Or a device may be present but lack something the kernel needs, which
/// on this project means double precision on a build that was not configured for
/// single. Collapsing all three into a thrown exception at construction would
/// make the common case, a laptop with no discrete GPU running the CPU solver,
/// look like an error.
///
/// So discovery answers with an optional description and never throws. A caller
/// that wants a GPU asks, and falls back to a CPU solver when the answer is
/// empty. `docs/performance/sycl_direct.md` reports the description of the
/// machine its figures were taken on, which is the same struct printed.
///
/// ## The allocation aspects are evidence, not decoration
///
/// Section 2 of the implementation plan rests an architectural claim on this
/// hardware having no host-to-device copy, and Phase 9's definition of done asks
/// for that to be demonstrated rather than asserted. The four USM aspects below
/// are the runtime's own answers about what this device can address, read rather
/// than inferred from the fact that the part is integrated, and they are
/// reported alongside every figure this backend produces.
///
/// The one that decides the backend's shape is `supports_system_usm`. A device
/// with it can dereference memory from an ordinary `new` or `malloc`, which
/// would let a kernel read `core::ParticleData`'s arrays where they lie and
/// remove the staging step in the solver entirely. The target GPU reports it
/// false, which is why the staging step exists. ADR-0027 records the
/// consequence, and this is the query behind it rather than an assumption about
/// integrated parts in general.
///
/// SYCL 2020 deprecated `info::device::host_unified_memory`, the flag that would
/// otherwise be the obvious thing to ask, in favour of exactly these aspects.
/// The project's builds treat warnings as errors, so the deprecated descriptor
/// is not queried, and nothing is lost: what it reported is what the aspects
/// report between them, in more detail.

#include <cstdint>
#include <optional>
#include <string>

namespace orrery::backend {

/// Whether this build contains the SYCL backend at all.
///
/// A constant rather than a function because it describes the translation unit
/// that reads it, exactly as `core::kSinglePrecision` does. A test that skips
/// its GPU cases needs the answer at compile time so that the cases it skips do
/// not have to link against a solver that was never built.
inline constexpr bool kSyclBackendCompiled =
#ifdef ORRERY_ENABLE_SYCL
    true;
#else
    false;
#endif

/// What the runtime reports about a device, in types that do not require SYCL.
///
/// Deliberately plain data. It crosses out of the SYCL translation units into
/// benchmark tables, test messages and the performance document, none of which
/// should have to include a device runtime header to print a device name.
struct DeviceDescription {
    /// The device's own name, for example "Intel(R) Arc(TM) 130V GPU (16GB)".
    std::string name;

    std::string vendor;

    /// The driver version, which belongs beside any performance figure. A
    /// graphics driver update can move an integrated GPU's throughput
    /// substantially, and a number quoted without it cannot be reproduced.
    std::string driver_version;

    /// Which runtime the device was reached through, "Level Zero" or "OpenCL".
    ///
    /// Both can expose the same physical GPU, and they do not perform
    /// identically. Reported so that a figure says which one produced it.
    std::string backend;

    /// Compute units, which is not the same count as Xe-cores.
    ///
    /// Section 2 of the implementation plan records 7 Xe-cores for this part and
    /// the runtime reports 64, because it counts the vector engines inside them
    /// rather than the cores. Both numbers are correct about different things,
    /// and this one is recorded under the runtime's name for it so that nobody
    /// reconciles them by assuming one is wrong.
    unsigned compute_units{};

    /// The largest work-group the device will accept.
    unsigned max_work_group_size{};

    /// The sub-group size the kernel was compiled for, which on Intel Xe is the
    /// SIMD width the hardware actually executes with.
    unsigned sub_group_size{};

    std::uint64_t global_memory_bytes{};

    /// Shared local memory per work-group, which is what a tiled kernel has to
    /// fit its block of source particles into.
    std::uint64_t local_memory_bytes{};

    /// Whether the device can allocate shared unified memory, addressable from
    /// both host and device. What this backend allocates.
    bool supports_shared_usm{};

    /// Whether the device can allocate memory only it can address. True on
    /// essentially everything, and not useful on a part with no separate memory
    /// for such an allocation to live in.
    bool supports_device_usm{};

    /// Whether the device can address host allocations.
    bool supports_host_usm{};

    /// Whether the device can address memory from an ordinary `new` or
    /// `malloc`, with no USM allocator involved at all.
    ///
    /// The aspect that would let a kernel read the particle arrays where the
    /// rest of the project already keeps them. False on the target GPU's
    /// driver, which is the measured fact ADR-0027 turns on.
    bool supports_system_usm{};

    /// Whether the device implements double precision.
    ///
    /// True on the target GPU, which is worth stating because the opposite is
    /// widely assumed of integrated Intel parts. What the aspect does not say is
    /// at what rate: reporting fp64 and executing it at a useful fraction of the
    /// single-precision rate are different claims, and only the second one
    /// matters to a simulation. `docs/performance/sycl_direct.md` measures it
    /// rather than inferring it from this flag.
    bool supports_double_precision{};

    /// True when this device can run the kernel in the precision this build was
    /// configured for.
    ///
    /// A single-precision build asks nothing unusual of any GPU. A
    /// double-precision build asks for `fp64`, which this project's target
    /// hardware does report, so the solver constructs in either configuration
    /// and the choice between them is made on measured throughput rather than on
    /// capability.
    [[nodiscard]] bool supports_build_precision() const noexcept;
};

/// The GPU this machine offers, or nothing.
///
/// Returns empty when the backend was not compiled, when the runtime reports no
/// device, when no device is a GPU, or when the runtime throws during discovery,
/// which it does on a machine whose driver is installed but broken. All four are
/// the same answer to the caller's actual question, which is whether there is a
/// GPU to run on.
///
/// Never throws. Discovery is the one operation that has to work on a machine
/// where nothing else does.
[[nodiscard]] std::optional<DeviceDescription> discover_gpu_device() noexcept;

/// Every device the runtime can see, GPU or not.
///
/// For the diagnostic case rather than the running case: when
/// `discover_gpu_device` returns nothing, this is what says whether the runtime
/// found nothing at all or found only a CPU device, and those point at different
/// problems. Empty when the backend was not compiled.
[[nodiscard]] std::optional<DeviceDescription> describe_default_device() noexcept;

/// A one-line summary, for a benchmark table or a test failure message.
[[nodiscard]] std::string to_string(const DeviceDescription& device);

} // namespace orrery::backend
