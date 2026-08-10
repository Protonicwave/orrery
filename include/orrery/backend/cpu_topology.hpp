#pragma once

/// \file
/// Which logical processors this machine has, and which of them are fast.
///
/// Section 2 of the implementation plan describes the target as four Lion Cove
/// performance cores beside four Skymont efficiency cores, and section 7 asks
/// Phase 6 to quantify the performance core idle time under static and dynamic
/// partitioning. That measurement is only possible if a worker thread can be
/// attributed to a class of core, which needs two things the standard library
/// does not offer: a way to ask the operating system what kind of core each
/// logical processor is, and a way to keep a thread on the one it started on.
///
/// Both are platform interfaces, so both are declared here and implemented per
/// platform in the source file. Neither is on any hot path: the topology is
/// queried once when a pool is built, and a thread is pinned once when it
/// starts.
///
/// The classification is deliberately coarse. This is not a general topology
/// library, and it does not report caches, packages or NUMA nodes, because
/// nothing in this project has a use for them on a single-socket laptop part.
/// It answers the one question Phase 6 asks and stops.
///
/// Where the platform cannot answer, the answer is `kUnknown` rather than a
/// guess. A benchmark that reported an efficiency core as a performance core
/// would produce exactly the wrong conclusion about the scheduler, so an honest
/// absence is worth more than a plausible default.

#include <cstdint>
#include <string_view>
#include <vector>

namespace orrery::backend {

/// What kind of core a logical processor belongs to.
///
/// Two classes rather than a numeric capability, because that is the shape of
/// the hardware this project targets and of what the operating systems report.
/// A part with three tiers, or one whose classes cannot be distinguished, is
/// described by `kUnknown` rather than by stretching this enumeration.
/// The base type is named rather than left to the compiler's default of `int`,
/// which would be four bytes for three values. It is stored one per logical
/// processor and one per worker, so the size is of no consequence at all here;
/// it is spelled out because the lint set asks for it and because a narrow
/// enumeration is the right habit in a project whose data layout decisions are
/// load bearing elsewhere.
enum class CoreClass : std::uint8_t {
    /// The platform does not distinguish, or the machine is homogeneous.
    kUnknown,
    /// The slower, smaller core. Skymont on the target machine.
    kEfficiency,
    /// The faster, larger core. Lion Cove on the target machine.
    kPerformance,
};

/// One logical processor, as the operating system numbers them.
struct LogicalProcessor {
    /// The index used to pin a thread, which is the platform's own numbering.
    unsigned id{};

    CoreClass core_class{CoreClass::kUnknown};
};

/// Every logical processor available to this process, in platform order.
///
/// Returns an empty vector if the platform cannot be asked, which a caller
/// should treat as "schedule normally and do not attribute by core class"
/// rather than as an error. There is nothing a simulation can usefully do about
/// not knowing, and refusing to run would be worse than running unattributed.
[[nodiscard]] std::vector<LogicalProcessor> query_logical_processors();

/// Confine the calling thread to one logical processor.
///
/// Returns false if the platform offers no way to do it, or if the request was
/// refused. Pinning is a measurement instrument here rather than a performance
/// technique: the operating system's own scheduler knows about hybrid cores and
/// generally places threads better than a fixed assignment does, so the pools
/// in this project leave it off unless a caller is measuring per-core-class
/// behaviour and needs a worker to stay put.
[[nodiscard]] bool pin_current_thread(unsigned processor) noexcept;

/// A short name for a core class, for benchmark tables and reports.
[[nodiscard]] std::string_view to_string(CoreClass core_class) noexcept;

} // namespace orrery::backend
