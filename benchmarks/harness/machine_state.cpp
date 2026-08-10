#include "harness/machine_state.hpp"

#include <array>
#include <cstddef>
#include <ctime>
#include <ostream>
#include <string>

#include "orrery/backend/cpu_features.hpp"
#include "orrery/backend/cpu_topology.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/core/build_info.hpp"
#include "orrery/core/types.hpp"

// The build type is not discoverable from inside a translation unit, so CMake
// passes it in. A build that somehow reached here without it says so rather
// than claiming a default, because "release" is exactly the wrong thing to
// guess.
#ifndef ORRERY_BUILD_TYPE
#    define ORRERY_BUILD_TYPE "unrecorded"
#endif

namespace orrery::benchmark {

namespace {

/// The compiler's own account of itself.
///
/// Each of the three supported compilers advertises its identity through a
/// different set of macros, and the order of the tests matters: Clang defines
/// the GNU macros as well when it is imitating GCC, and clang-cl defines
/// `_MSC_VER`, so Clang has to be asked first or it is reported as something
/// else.
[[nodiscard]] std::string compiler_description() {
#ifdef __clang__
    return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) +
           "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
           std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown compiler";
#endif
}

/// The local date and time, to the second.
///
/// Through the platform's reentrant conversion rather than `std::localtime`,
/// which returns a pointer to a shared buffer and is therefore not safe to call
/// from more than one thread. Nothing here is threaded today; the version that
/// cannot become wrong is the same length.
[[nodiscard]] std::string local_timestamp() {
    const std::time_t now = std::time(nullptr);

    std::tm broken_down{};
#ifdef _WIN32
    if (localtime_s(&broken_down, &now) != 0) {
        return "unknown";
    }
#else
    if (localtime_r(&now, &broken_down) == nullptr) {
        return "unknown";
    }
#endif

    std::array<char, 32> text{};
    const std::size_t written =
        std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S", &broken_down);

    // A zero return means the buffer was too small, which for a fixed format of
    // nineteen characters in thirty-two bytes cannot happen. It is checked
    // because the alternative is reading whatever the buffer held.
    return written == 0 ? std::string{"unknown"} : std::string{text.data(), written};
}

} // namespace

MachineState capture_machine_state() {
    MachineState state;

    state.processor = std::string{backend::cpu_brand()};
    state.logical_processors = backend::ThreadPool::default_worker_count();

    for (const backend::LogicalProcessor& processor : backend::query_logical_processors()) {
        if (processor.core_class == backend::CoreClass::kPerformance) {
            ++state.performance_cores;
        } else if (processor.core_class == backend::CoreClass::kEfficiency) {
            ++state.efficiency_cores;
        }
    }

    state.avx2 = backend::cpu_features().avx2;
    state.fma = backend::cpu_features().fma;

    state.compiler = compiler_description();
    state.build_type = ORRERY_BUILD_TYPE;
    state.scalar = core::kSinglePrecision ? "float" : "double";
    state.version = std::string{core::version()};
    state.taken_at = local_timestamp();

    return state;
}

void print(std::ostream& out, const MachineState& state) {
    out << "processor:  " << (state.processor.empty() ? "unreported" : state.processor) << '\n'
        << "cores:      " << state.logical_processors << " logical";

    if (state.performance_cores > 0 || state.efficiency_cores > 0) {
        out << ", " << state.performance_cores << " performance and " << state.efficiency_cores
            << " efficiency";
    } else {
        out << ", topology not reported as hybrid";
    }

    out << "\nvectors:    avx2 " << (state.avx2 ? "yes" : "no") << ", fma "
        << (state.fma ? "yes" : "no") << '\n'
        << "build:      Orrery " << state.version << ", " << state.build_type << ", "
        << state.compiler << ", scalar " << state.scalar << '\n'
        << "taken at:   " << state.taken_at << '\n';
}

} // namespace orrery::benchmark
