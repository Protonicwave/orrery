#include "orrery/backend/cpu_topology.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

#ifdef _WIN32
// WIN32_LEAN_AND_MEAN and NOMINMAX are set on this target in CMake rather than
// defined here, so that the convention against macros in section 4 of the
// implementation plan holds in the source even where the platform header
// requires them.
//
// NOLINTNEXTLINE(misc-include-cleaner)
#    include <windows.h>
#elif defined(__linux__)
#    include <fstream>
#    include <string>
#    include <thread>

#    include <pthread.h>
#    include <sched.h>
#endif

namespace orrery::backend {

// Only the platforms that can answer the question compile the helper that
// shapes the answer. On macOS and the BSDs neither implementation below is
// compiled, so an unconditional definition here would be an unused function,
// which this project's warning set correctly treats as an error.
#if defined(_WIN32) || defined(__linux__)

namespace {

/// Turn a capability ranking into core classes.
///
/// Both platforms answer the same question in different units, Windows with an
/// efficiency class and Linux with a maximum clock frequency, and in both a
/// larger number means a more capable core. Splitting them here means the
/// comparison rule is written once and the platform code only has to produce
/// the numbers.
///
/// Everything at the top rank is a performance core and everything below it an
/// efficiency core. On a part with three tiers that would put the middle tier
/// with the efficiency cores, which is a simplification rather than a mistake:
/// the target machine has exactly two tiers, and a machine with three would
/// need the reports in `docs/performance/` rewritten anyway.
[[nodiscard]] std::vector<LogicalProcessor> classify(const std::vector<unsigned>& ids,
                                                     const std::vector<unsigned long long>& ranks) {
    std::vector<LogicalProcessor> processors;
    processors.reserve(ids.size());

    const auto [lowest, highest] = std::ranges::minmax_element(ranks);

    // A machine whose cores all rank the same is homogeneous as far as this
    // question goes, and so is one where the ranking could not be read at all
    // and every entry is zero. Neither is a hybrid part, and calling every core
    // a performance core would invite a report that quietly claimed to have
    // measured a distinction that does not exist here.
    const bool hybrid = lowest != ranks.end() && *lowest != *highest;

    for (std::size_t index = 0; index < ids.size(); ++index) {
        CoreClass core_class = CoreClass::kUnknown;
        if (hybrid) {
            core_class =
                ranks[index] == *highest ? CoreClass::kPerformance : CoreClass::kEfficiency;
        }
        processors.push_back(LogicalProcessor{.id = ids[index], .core_class = core_class});
    }

    return processors;
}

} // namespace

#endif

#ifdef _WIN32

// The Windows implementation is exempted from three checks for the length of
// the block, because all three object to the shape of the platform interface
// rather than to anything this project chose.
//
// misc-include-cleaner wants the specific SDK header that declares each symbol.
// windows.h is an umbrella over several dozen of them and including its parts
// individually is not how the platform is meant to be used.
//
// The union access is how SYSTEM_CPU_SET_INFORMATION is defined: one tagged
// union discriminated by its Type field, which is checked before the member is
// read.
//
// The reinterpret casts walk a buffer of variable-length records, which is the
// only way the interface offers to read its reply.
//
// NOLINTBEGIN(misc-include-cleaner,cppcoreguidelines-pro-type-union-access,cppcoreguidelines-pro-type-reinterpret-cast)

std::vector<LogicalProcessor> query_logical_processors() {
    // The CPU set interface rather than GetLogicalProcessorInformationEx,
    // because it is the one that reports an efficiency class per processor.
    // That field is what Windows itself uses to place threads on a hybrid part,
    // so it is the platform's own answer to the question rather than an
    // inference from clock speeds.
    ULONG bytes = 0;
    (void)GetSystemCpuSetInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0);
    if (bytes == 0) {
        return {};
    }

    // A vector of the structure rather than of bytes, so that the storage is
    // aligned for it. The records are variable length in principle, which is
    // why the walk below advances by each record's own Size rather than by
    // sizeof, and the buffer is rounded up to a whole number of them.
    std::vector<SYSTEM_CPU_SET_INFORMATION> buffer(
        (bytes + sizeof(SYSTEM_CPU_SET_INFORMATION) - 1) / sizeof(SYSTEM_CPU_SET_INFORMATION));

    if (GetSystemCpuSetInformation(buffer.data(), bytes, &bytes, GetCurrentProcess(), 0) == FALSE) {
        return {};
    }

    std::vector<unsigned> ids;
    std::vector<unsigned long long> ranks;

    const auto* cursor = reinterpret_cast<const std::byte*>(buffer.data());
    const auto* last = cursor + bytes;

    while (cursor + sizeof(SYSTEM_CPU_SET_INFORMATION) <= last) {
        const auto* record = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(cursor);

        if (record->Size == 0) {
            // A zero length would not advance the cursor, so the walk would not
            // terminate. The interface does not produce one, and this is here
            // so that a malformed reply is a short answer rather than a hang.
            break;
        }

        if (record->Type == CpuSetInformation) {
            ids.push_back(record->CpuSet.LogicalProcessorIndex);
            ranks.push_back(record->CpuSet.EfficiencyClass);
        }

        cursor += record->Size;
    }

    return classify(ids, ranks);
}

bool pin_current_thread(unsigned processor) noexcept {
    // An affinity mask addresses one processor group, which holds at most 64
    // logical processors. Machines larger than that exist and this project does
    // not run on them: the target part has eight. A request beyond the group is
    // refused rather than silently wrapped onto the wrong core.
    if (processor >= sizeof(DWORD_PTR) * 8) {
        return false;
    }

    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << processor;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
}

// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-type-union-access,cppcoreguidelines-pro-type-reinterpret-cast)

#elif defined(__linux__)

std::vector<LogicalProcessor> query_logical_processors() {
    const unsigned count = std::thread::hardware_concurrency();
    if (count == 0) {
        return {};
    }

    std::vector<unsigned> ids;
    std::vector<unsigned long long> ranks;
    ids.reserve(count);
    ranks.reserve(count);

    for (unsigned cpu = 0; cpu < count; ++cpu) {
        // The maximum clock frequency stands in for the efficiency class, which
        // Linux does not export in a form that is stable across kernel
        // versions. On a hybrid Intel part the two core types have different
        // maximum frequencies, so the ranking separates them. Where cpufreq is
        // absent, every reading is zero, and `classify` reports the machine as
        // undistinguished rather than inventing a split.
        const std::string path =
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/cpuinfo_max_freq";
        std::ifstream file{path};

        unsigned long long kilohertz = 0;
        if (!(file >> kilohertz)) {
            kilohertz = 0;
        }

        ids.push_back(cpu);
        ranks.push_back(kilohertz);
    }

    return classify(ids, ranks);
}

bool pin_current_thread(unsigned processor) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(processor, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

#else

std::vector<LogicalProcessor> query_logical_processors() {
    // macOS and the BSDs have no supported interface for either question. macOS
    // in particular deliberately refuses to let a process pin a thread, because
    // the placement of work across its own performance and efficiency cores is
    // the kernel's to decide. Reporting nothing is the truthful answer.
    return {};
}

bool pin_current_thread(unsigned /*processor*/) noexcept {
    return false;
}

#endif

std::string_view to_string(CoreClass core_class) noexcept {
    switch (core_class) {
    case CoreClass::kEfficiency:
        return "efficiency";
    case CoreClass::kPerformance:
        return "performance";
    case CoreClass::kUnknown:
        break;
    }
    return "unknown";
}

} // namespace orrery::backend
