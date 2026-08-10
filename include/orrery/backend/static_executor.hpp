#pragma once

/// \file
/// Equal fixed shares, one per worker. The scheme this phase exists to reject.
///
/// It is the obvious way to parallelise a loop and it is what most codes do:
/// divide the range into as many equal pieces as there are threads, give each
/// thread one, wait for them all. On a machine whose cores are alike it is very
/// nearly optimal, and it has no scheduling overhead at all, because there is no
/// scheduling. Each worker is told once what to do and does it.
///
/// On the target machine it is the wrong answer, for the reason section 2 of the
/// implementation plan sets out. Four Lion Cove performance cores and four
/// Skymont efficiency cores do not run the same code at the same rate, so equal
/// shares take unequal times, and the region cannot end until the slowest
/// worker finishes. The performance cores complete their share and then wait,
/// and the whole of that waiting is throughput the machine had available and did
/// not use.
///
/// This class is kept, and kept correct, because the claim that dynamic
/// scheduling is worth its overhead is only worth making if the alternative was
/// measured rather than assumed. `docs/performance/threading.md` reports both,
/// and ADR-0016 records the decision that follows from the numbers. It is also
/// the honest baseline for a machine that is not hybrid, where it should win.

#include <cstdint>
#include <string_view>
#include <vector>

#include "orrery/backend/cpu_topology.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/types.hpp"

namespace orrery::backend {

/// Divides the range into equal shares once and does not revisit the decision.
class StaticExecutor final : public Executor {
public:
    /// One worker per logical processor unless told otherwise.
    ///
    /// `affinity` is a measurement instrument rather than a tuning knob; see
    /// `ThreadPool::Affinity`.
    explicit StaticExecutor(unsigned worker_count = 0,
                            ThreadPool::Affinity affinity = ThreadPool::Affinity::kUnpinned);

    void run(core::Index count, RangeTask task) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "static"; }

    [[nodiscard]] unsigned worker_count() const noexcept override { return pool_.worker_count(); }

    [[nodiscard]] CoreClass worker_core_class(unsigned worker) const noexcept override {
        return pool_.core_class(worker);
    }

    [[nodiscard]] ExecutorStatistics statistics() const override;

    void reset_statistics() noexcept override;

private:
    ThreadPool pool_;

    /// Declared after the pool so that the workers exist before anything sized
    /// from their number, and sized once so that no region allocates.
    std::vector<PaddedWorkerStatistics> workers_;

    /// Each worker's accumulated busy time as the current region began, so that
    /// the region's own busy time can be recovered afterwards without the
    /// workers having to write a second counter while they run.
    std::vector<Duration> busy_at_region_start_;

    Duration elapsed_{};
    std::uint64_t regions_{};
};

} // namespace orrery::backend
