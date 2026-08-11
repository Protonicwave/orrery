#include "orrery/sim/run_output.hpp"

#include <span>

#include "orrery/core/types.hpp"
#include "orrery/sim/checkpoint.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/simulation.hpp"

namespace orrery::sim {

FileOutput::FileOutput(const Configuration& configuration, std::span<const core::Real> masses)
    : configuration_(configuration), checkpoint_path_(configuration.output.checkpoint_path) {
    const OutputSettings& output = configuration.output;

    if (!output.trajectory_path.empty()) {
        trajectory_.emplace(output.trajectory_path, masses, configuration.run.timestep,
                            output.trajectory_velocities);
    }
    if (!output.diagnostics_path.empty()) {
        diagnostics_.emplace(output.diagnostics_path);
    }
}

bool FileOutput::due(core::Index step, core::Index stride, bool is_final) const noexcept {
    if (first_ || is_final) {
        return true;
    }
    return stride != 0 && step % stride == 0;
}

core::Index FileOutput::frames_written() const noexcept {
    return trajectory_ ? trajectory_->frame_count() : 0;
}

void FileOutput::record(const Simulation& simulation, bool is_final) {
    const OutputSettings& output = configuration_.output;
    const core::Index step = simulation.step_index();

    if (trajectory_ && due(step, output.trajectory_stride, is_final)) {
        trajectory_->write_frame(step, simulation.time(), simulation.particles());
    }

    if (diagnostics_ && due(step, output.diagnostics_stride, is_final)) {
        diagnostics_->record(step, simulation.time(), simulation.measure());
    }

    if (!checkpoint_path_.empty() && due(step, output.checkpoint_stride, is_final)) {
        Checkpoint checkpoint;
        checkpoint.configuration = configuration_;
        checkpoint.step = step;
        checkpoint.particles = simulation.particles();
        write_checkpoint(checkpoint_path_, checkpoint);
        ++checkpoints_;

        // Flushed together with the checkpoint rather than left to the operating
        // system, so that a process killed just after a checkpoint leaves a
        // trajectory and a diagnostics file that reach at least as far as the
        // state the run would resume from. The other way round, a resumed run
        // would append frames the files already held.
        if (trajectory_) {
            trajectory_->flush();
        }
        if (diagnostics_) {
            diagnostics_->flush();
        }
    }

    first_ = false;
}

} // namespace orrery::sim
