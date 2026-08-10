#pragma once

/// \file
/// How a kernel asks for a loop to be run in parallel.
///
/// Every parallelisable kernel in this project has the same shape. There is a
/// range of particle indices, the work at each index is independent of the work
/// at every other, and each index writes only its own outputs. ADR-0015 made
/// that true of the direct solver on purpose, by having each particle read every
/// other and write only itself, and Phase 8's tree walk will have the same
/// property for the same reason. So the whole of what the kernels need from a
/// scheduler is: divide `[0, count)` among some threads and call this for each
/// piece.
///
/// That is the entire interface. There is no task graph, no dependency
/// tracking, no nested spawning and no future to wait on, because no kernel here
/// needs any of them, and a scheduler general enough to express them would be
/// harder to reason about and slower at the one thing it actually does.
///
/// The implementations differ in how they divide the range, which is precisely
/// the question Phase 6 was written to answer, and they are selected at run time
/// so that a benchmark can measure one against another in the same process on
/// the same configuration. The virtual call happens once per force evaluation,
/// ahead of N^2 interactions, which is the boundary section 3 of the
/// implementation plan permits dispatch at.
///
/// A task must not throw. Section 4 forbids an exception leaving a kernel, and
/// here the rule has teeth beyond style: the task runs on a worker thread, and
/// an exception escaping it would cross a thread boundary with nowhere to be
/// caught. The worker entry points are `noexcept`, so such an exception
/// terminates the program at the point it was thrown rather than corrupting the
/// pool's bookkeeping on the way out.

#include <string_view>

#include "orrery/backend/cpu_topology.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/function_ref.hpp"
#include "orrery/core/types.hpp"

namespace orrery::backend {

/// A piece of a parallel loop: the half-open index range `[begin, end)`.
///
/// A reference rather than a `std::function` because it is called across a
/// virtual boundary once per force evaluation and owns nothing that outlives the
/// call. `core/function_ref.hpp` sets out the reasoning and the lifetime rule
/// that comes with it.
using RangeTask = core::FunctionRef<void(core::Index, core::Index)>;

/// Something that can run a loop body over a range, possibly in parallel.
class Executor {
public:
    virtual ~Executor() = default;

    /// Call `task` over pieces that together cover `[0, count)` exactly once
    /// each, and return when all of them have finished.
    ///
    /// The division is the implementation's to choose, and so is the number of
    /// threads it uses and the order the pieces run in. A caller may rely on
    /// each index being covered exactly once and on all the work being complete
    /// on return, and on nothing else.
    ///
    /// A count of zero runs nothing. An empty configuration is a configuration,
    /// and it reaches the solvers by the same path as any other.
    virtual void run(core::Index count, RangeTask task) = 0;

    /// The scheme's name, for benchmark tables and reports.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// How many workers this executor divides work between.
    [[nodiscard]] virtual unsigned worker_count() const noexcept = 0;

    /// What kind of core a worker runs on, where that is known.
    ///
    /// `kUnknown` unless the executor pinned its workers, because without
    /// pinning the operating system is free to move a thread between a
    /// performance and an efficiency core mid-region, and an answer that
    /// described where the thread started would be a guess about where it spent
    /// its time. Phase 6's per-core-class figures come from pinned runs for
    /// exactly this reason.
    [[nodiscard]] virtual CoreClass worker_core_class(unsigned worker) const noexcept = 0;

    /// The record of what the workers have done since the last reset.
    ///
    /// Safe to call only when no region is running, which for a single
    /// submitting thread means any time `run` is not on the stack. It copies
    /// the per-worker records rather than returning a view of them, because a
    /// caller comparing two schemes needs the first scheme's numbers to survive
    /// the second scheme running.
    [[nodiscard]] virtual ExecutorStatistics statistics() const = 0;

    /// Set every counter back to zero.
    ///
    /// A benchmark measures the region it timed rather than the warm-up before
    /// it, and once the two have been added together they cannot be separated.
    virtual void reset_statistics() noexcept = 0;

protected:
    // As on the other interfaces in this project: a class with a virtual
    // destructor suppresses the implicit copy and move operations, and copying
    // an implementation through a reference to this base would slice it.
    Executor() = default;
    Executor(const Executor&) = default;
    Executor(Executor&&) = default;
    Executor& operator=(const Executor&) = default;
    Executor& operator=(Executor&&) = default;
};

} // namespace orrery::backend
