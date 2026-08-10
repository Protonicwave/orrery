#pragma once

/// \file
/// What was measured, on what, built how, and when.
///
/// Section 7 of the implementation plan asks Phase 7's harness to record the
/// machine state alongside the numbers. This is not bookkeeping. Every
/// performance figure in this project is a statement about a particular
/// processor running a particular build, and a figure filed without them is not
/// reproducible and therefore, by the standard section 5 sets, not a result.
///
/// The specific failures this guards against have all happened to somebody.
/// A timing quoted from a debug build. A single-precision figure compared
/// against a double-precision one. A speedup that turned out to be against a
/// baseline on the wrong kind of core, which is the mistake Phase 6 made and
/// caught. A regression that was a compiler upgrade. Each is invisible in the
/// number and obvious in the state that produced it.
///
/// What is recorded is what changes the answer and can be discovered from
/// inside the process. Temperature, power profile and what else was running
/// cannot be, on any portable interface, so they are not silently omitted:
/// `docs/performance/` states them for each session by hand, and the thermal
/// canary in `protocol.hpp` measures the one consequence of them that matters.

#include <iosfwd>
#include <string>

namespace orrery::benchmark {

/// Everything about a run that a reader would need in order to repeat it.
struct MachineState {
    /// The processor's own name for itself, or empty where it offers none.
    std::string processor;

    /// Logical processors the operating system reports.
    unsigned logical_processors{};

    /// How many of them are of each kind, where the platform distinguishes.
    ///
    /// Zero and zero on a machine that does not report a hybrid topology, which
    /// is a different statement from four and four and is printed as such. The
    /// asymmetry between these two counts is the single most important property
    /// of the target machine (ADR-0016), so a report that could not tell
    /// whether it was present would be missing the point.
    unsigned performance_cores{};
    unsigned efficiency_cores{};

    /// Whether the vector kernels of this phase can run here at all.
    bool avx2{};
    bool fma{};

    /// The compiler and its version, as the compiler itself reports them.
    std::string compiler;

    /// The CMake build type, which decides the optimisation level.
    ///
    /// Recorded because a timing from a debug build is the single most common
    /// way a performance claim turns out to be worthless, and because nothing
    /// in the numbers themselves reveals it.
    std::string build_type;

    /// `double` or `float`, from the precision the build was configured with.
    std::string scalar;

    /// The project version, from `core/build_info.hpp`.
    std::string version;

    /// Local date and time the state was captured.
    std::string taken_at;
};

/// Ask the machine and the build about themselves.
[[nodiscard]] MachineState capture_machine_state();

/// Write the state as a block of labelled lines.
///
/// Plain text rather than a structured format. These reports are read by people
/// and pasted into `docs/performance/`, and a benchmark that emitted JSON for a
/// human to reformat would have made the wrong trade.
void print(std::ostream& out, const MachineState& state);

} // namespace orrery::benchmark
