#pragma once

/// \file
/// Finding an NVIDIA GPU, and reporting what was found.
///
/// The counterpart of `backend/sycl_device.hpp`, and deliberately not a
/// generalisation of it. Writing one `GpuDescription` that both backends filled
/// in was the obvious first move and it was rejected, for a reason ADR-0060 sets
/// out at length and which this file is the evidence for: the two runtimes do
/// not describe the same machine. SYCL reports compute units, sub-group sizes
/// and four unified
/// memory aspects. CUDA reports multiprocessors, a warp size, a compute
/// capability and whether the part is integrated. A struct holding the union
/// would have half its fields empty whichever backend filled it, and a reader
/// could not tell an unsupported feature from an unasked question.
///
/// So there are two descriptions, in each vendor's own vocabulary, and the
/// document that compares them does the translating in prose where a reader can
/// see it (`docs/performance/cuda.md`).
///
/// ## Discovery is a query rather than a constructor, for the same three reasons
///
/// `backend/sycl_device.hpp` sets them out and they carry over unchanged. The
/// build may not include the backend. The build may include it while the
/// machine has no device, which is the ordinary state of a continuous
/// integration runner and is the state this project's own CUDA job is in on
/// purpose. Or a device may be present but unusable, which on CUDA usually
/// means a driver older than the runtime the binary was linked against.
///
/// All three are the same answer to the caller's question, which is whether
/// there is a device to run on, so discovery returns an optional description and
/// never throws.
///
/// ## The field that matters most here is `integrated`
///
/// Phase 9 rested an architectural claim on the target GPU sharing physical
/// memory with the CPU: no host-to-device copy exists to elide, and the staging
/// the solver performs is host memory to host memory (ADR-0027). That claim was
/// measured rather than assumed, through the USM aspects.
///
/// This backend runs on hardware where the same claim is false. A discrete card
/// has its own memory across a bus, every byte the kernel reads has to be sent
/// there, and the transfer is a real cost rather than a copy that could in
/// principle be removed. `integrated` is the runtime's own answer to that
/// question rather than an inference from the device's name, and it is reported
/// beside every figure this backend produces, because a throughput number from a
/// part with 320 GB/s of its own memory and one from a part sharing 95 GB/s with
/// eight CPU cores are not the same measurement.

#include <cstdint>
#include <optional>
#include <string>

namespace orrery::backend {

/// Whether this build contains the CUDA backend at all.
///
/// A constant rather than a function, for the reason `kSyclBackendCompiled`
/// gives: it describes the translation unit that reads it, so a test that skips
/// its device cases needs the answer at compile time rather than at run time.
inline constexpr bool kCudaBackendCompiled =
#ifdef ORRERY_ENABLE_CUDA
    true;
#else
    false;
#endif

/// What the CUDA runtime reports about a device, in types that do not require
/// the toolkit.
///
/// Plain data, on the same terms as `DeviceDescription`: it crosses out of the
/// translation units that include `<cuda_runtime.h>` into benchmark tables, test
/// messages and the performance document, none of which should have to find a
/// toolkit in order to print a device name.
struct CudaDeviceDescription {
    /// The device's own name, for example "Tesla T4".
    std::string name;

    /// The compute capability, as a pair rather than a formatted string.
    ///
    /// This is the number that decides whether the binary can run at all. A
    /// build compiles device images for the architectures named in
    /// `ORRERY_CUDA_ARCHITECTURES`, and a card whose capability has no image and
    /// no compatible PTX to compile from will refuse the launch. Reported so
    /// that the refusal is diagnosable from the table rather than from a driver
    /// error code.
    unsigned compute_capability_major{};
    unsigned compute_capability_minor{};

    /// Streaming multiprocessors, which is the count that plays the part
    /// `DeviceDescription::compute_units` plays on the other backend without
    /// being the same quantity.
    ///
    /// The SYCL runtime counts vector engines inside Xe-cores and reports 64 for
    /// a part with 7 cores. CUDA counts the multiprocessors themselves. The two
    /// numbers must not be compared, and they are kept under each vendor's own
    /// name so that nobody is tempted to.
    unsigned multiprocessor_count{};

    /// The warp width, which is 32 on every NVIDIA part shipped so far.
    ///
    /// The exact counterpart of `DeviceDescription::sub_group_size`, and the
    /// quantity the coherent tree traversal of ADR-0029 is built around: it is
    /// how many targets agree to walk the tree together. That the two vendors
    /// happen to agree on 32 is a convenience rather than a guarantee, and the
    /// traversal reads it rather than assuming it.
    unsigned warp_size{};

    /// The largest block the device will accept.
    unsigned max_threads_per_block{};

    /// Shared memory per block, which is what a tiled kernel stages its sources
    /// through.
    std::uint64_t shared_memory_bytes_per_block{};

    /// The device's own memory.
    ///
    /// On a discrete card this is a hard ceiling that the integrated part did
    /// not have: the Intel GPU of Phase 9 reports the machine's whole 16 GB
    /// because it is the machine's memory, while this figure is a separate
    /// allocation pool the host cannot address.
    std::uint64_t global_memory_bytes{};

    /// Whether the GPU shares physical memory with the host.
    ///
    /// False on every discrete card and true on the integrated parts NVIDIA
    /// ships. The field this backend's whole staging story turns on, for the
    /// reason the file comment gives, and read from the runtime rather than
    /// inferred from the device's name.
    bool integrated{};

    /// Whether host and device share one virtual address space.
    ///
    /// True on essentially everything modern. Recorded because it is what makes
    /// `cudaPointerGetAttributes` able to answer what a pointer is, which is the
    /// query `backend/cuda_memory.hpp` uses to demonstrate where each of this
    /// backend's allocations actually lives.
    bool unified_addressing{};

    /// Whether the device can allocate memory the runtime migrates on demand.
    ///
    /// The nearest thing CUDA has to the shared unified memory Phase 9 used, and
    /// deliberately not what this backend allocates. ADR-0060 records why:
    /// managed memory on a discrete card does not remove the transfer, it hides
    /// it, and a solver that hid it could not report what it cost.
    bool supports_managed_memory{};

    /// The installed driver, as the runtime reports it.
    ///
    /// Belongs beside any performance figure for the reason
    /// `DeviceDescription::driver_version` gives: a driver update moves
    /// throughput, and a number quoted without one cannot be reproduced.
    unsigned driver_version{};

    /// The CUDA runtime the binary was built against.
    unsigned runtime_version{};

    /// True when this device can run the kernels in the precision this build was
    /// configured for.
    ///
    /// Every CUDA device implements double precision, so this is true in both
    /// configurations and the function exists to say so rather than to decide
    /// anything. It is kept because the SYCL description has one, because a
    /// caller written against both should ask the same question of each, and
    /// because the answer being uninteresting is itself worth recording: on this
    /// vendor the precision question is entirely about rate, and consumer parts
    /// run double precision at a small fraction of their single-precision rate
    /// while reporting full support for it. `docs/performance/cuda.md` measures
    /// the ratio rather than quoting a specification.
    [[nodiscard]] bool supports_build_precision() const noexcept;
};

/// The CUDA device this machine offers, or nothing.
///
/// Returns empty when the backend was not compiled, when the runtime reports no
/// device, when the driver is too old for the runtime, or when the runtime fails
/// for any other reason. All four are the same answer to the caller's actual
/// question, which is whether there is a device to run on.
///
/// The first device is taken where there are several. Choosing between them
/// would need a policy this project has no basis for: the machines it is
/// measured on have exactly one.
///
/// Never throws. Discovery is the one operation that has to work on a machine
/// where nothing else does.
[[nodiscard]] std::optional<CudaDeviceDescription> discover_cuda_device() noexcept;

/// How many CUDA devices the runtime can see, which is zero on a machine with
/// none and on a build without the backend.
///
/// For the diagnostic case rather than the running case: when
/// `discover_cuda_device` returns nothing, this says whether the runtime found
/// no devices or found one it could not describe, and those point at different
/// problems.
[[nodiscard]] unsigned cuda_device_count() noexcept;

/// A one-line summary, for a benchmark table or a test failure message.
[[nodiscard]] std::string to_string(const CudaDeviceDescription& device);

/// A version as the CUDA runtime encodes it, written the way people say it.
///
/// The runtime reports 12040 for 12.4. Exposed rather than kept private because
/// both version fields above are encoded that way and every caller that prints
/// one would otherwise repeat the arithmetic.
[[nodiscard]] std::string to_version_string(unsigned version);

} // namespace orrery::backend
