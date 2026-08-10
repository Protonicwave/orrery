#include "orrery/backend/serial_executor.hpp"

#include <chrono>
#include <cstdint>

#include "orrery/backend/executor.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/types.hpp"

namespace orrery::backend {

void SerialExecutor::run(core::Index count, RangeTask task) {
    if (count == 0) {
        return;
    }

    const Clock::time_point start = Clock::now();
    task(0, count);
    const auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start);

    elapsed_ += elapsed;
    ++regions_;

    // One worker, busy for the whole region and idle for none of it. The idle
    // time of a scheme with nothing to wait for is zero rather than undefined,
    // and saying so keeps the baseline row of a comparison table meaningful
    // instead of blank.
    worker_.busy += elapsed;
    worker_.chunks += 1;
    worker_.items += static_cast<std::uint64_t>(count);
}

ExecutorStatistics SerialExecutor::statistics() const {
    return ExecutorStatistics{.workers = {worker_}, .elapsed = elapsed_, .regions = regions_};
}

void SerialExecutor::reset_statistics() noexcept {
    worker_ = WorkerStatistics{};
    elapsed_ = Duration::zero();
    regions_ = 0;
}

} // namespace orrery::backend
