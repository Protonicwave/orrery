#pragma once

/// \file
/// The executor that does not thread at all.
///
/// It exists for two reasons, and neither is a placeholder.
///
/// The first is that it is the baseline. A speedup is a ratio, and the number
/// underneath it has to come from the same kernel compiled the same way and
/// called through the same interface, or the comparison is measuring the
/// difference between two code paths as well as the difference between one
/// thread and eight. Running the identical task function on the calling thread
/// is the only way to be sure of that.
///
/// The second is that threading is not always wanted. A unit test asserting an
/// exact acceleration has nothing to gain from eight threads and something to
/// lose in reproducibility of failure. A configuration of a few dozen particles
/// costs less to compute than to hand to a pool. And Phase 9's GPU backend will
/// want the CPU threads out of the way while it measures the device.
///
/// It reports statistics like any other executor, with one worker that is busy
/// for the whole region. Those numbers are not interesting on their own; they
/// are what makes the baseline appear in the same table as the schemes being
/// compared against it, rather than as a bare number in the surrounding text.

#include <cstdint>
#include <string_view>

#include "orrery/backend/cpu_topology.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/types.hpp"

namespace orrery::backend {

/// Runs the whole range on the calling thread.
class SerialExecutor final : public Executor {
public:
    SerialExecutor() = default;

    void run(core::Index count, RangeTask task) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "serial"; }

    [[nodiscard]] unsigned worker_count() const noexcept override { return 1; }

    /// Always unknown. The calling thread is not pinned, and pinning it would
    /// be a side effect on a thread this class does not own.
    [[nodiscard]] CoreClass worker_core_class(unsigned /*worker*/) const noexcept override {
        return CoreClass::kUnknown;
    }

    [[nodiscard]] ExecutorStatistics statistics() const override;

    void reset_statistics() noexcept override;

private:
    WorkerStatistics worker_;
    Duration elapsed_{};
    std::uint64_t regions_{};
};

} // namespace orrery::backend
