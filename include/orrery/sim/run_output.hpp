#pragma once

/// \file
/// Where a run's results go, behind one interface so that the simulation does
/// not know.
///
/// `Simulation::run` calls `record` once for the state it starts from and once
/// after every step. What that means is entirely the output's business: the
/// implementation here writes three files at three independent strides, the test
/// suite uses one that keeps the states in memory so that a resumed run can be
/// compared against an uninterrupted one bit for bit, and a renderer in a later
/// phase would use one that draws.
///
/// The alternative, a simulation that opened files itself, would put the file
/// formats on the include path of the class that owns the physics and would make
/// the resume test impossible to write without a disc.
///
/// ## Strides
///
/// Each output has its own, in steps, and zero means the two ends of the run
/// only. Writing every step is almost never right: the diagnostics cost an N^2
/// pass, and a trajectory of a million particles is 24 MB a frame in double
/// precision, so a run that recorded each step would spend more time writing
/// than integrating and would fill a disc in about a minute.

#include <filesystem>
#include <optional>
#include <span>

#include "orrery/core/types.hpp"
#include "orrery/sim/configuration.hpp"
#include "orrery/sim/diagnostics_log.hpp"
#include "orrery/sim/trajectory.hpp"

namespace orrery::sim {

class Simulation;

/// Somewhere for a run to report itself to.
class RunOutput {
public:
    virtual ~RunOutput() = default;

    /// Record the current state of `simulation`.
    ///
    /// Called once before the first step and once after each step, with
    /// `is_final` true only on the last of those. An implementation decides for
    /// itself which of those calls it acts on.
    ///
    /// May throw. This is called between steps rather than inside one, so the
    /// prohibition in section 4 of the implementation plan on exceptions leaving
    /// a kernel is not engaged, and a disc that has filled is something the
    /// person running the simulation has to be told about rather than something
    /// to carry on quietly without.
    virtual void record(const Simulation& simulation, bool is_final) = 0;

protected:
    // As on the other interfaces in this project: a class with a virtual
    // destructor suppresses the implicit copy and move operations, and copying
    // an implementation through a reference to this base would slice it.
    RunOutput() = default;
    RunOutput(const RunOutput&) = default;
    RunOutput(RunOutput&&) = default;
    RunOutput& operator=(const RunOutput&) = default;
    RunOutput& operator=(RunOutput&&) = default;
};

/// Writes the trajectory, the diagnostics and the checkpoints a configuration
/// asks for.
///
/// An output not given a path is not opened, so a run that names no files does
/// no I/O and pays for none of this.
class FileOutput final : public RunOutput {
public:
    /// Open what `configuration` asks for.
    ///
    /// The masses are needed here rather than at the first frame because they
    /// go in the trajectory header, which is written when the file is created.
    /// They do not change during a run, which is the reason the format puts them
    /// there.
    ///
    /// Throws `std::runtime_error` if a path cannot be opened. Deliberately at
    /// construction, which is before the first step, so that a run with an
    /// unwritable output directory stops immediately rather than after an hour
    /// of computing.
    FileOutput(const Configuration& configuration, std::span<const core::Real> masses);

    void record(const Simulation& simulation, bool is_final) override;

    /// How many trajectory frames have been written.
    [[nodiscard]] core::Index frames_written() const noexcept;

    /// How many checkpoints have been written.
    [[nodiscard]] core::Index checkpoints_written() const noexcept { return checkpoints_; }

private:
    /// Whether an output with this stride acts on this step.
    ///
    /// The two ends of a run always count, whatever the stride, so that a file
    /// records where the run started and where it finished even when nothing in
    /// between was asked for.
    [[nodiscard]] bool due(core::Index step, core::Index stride, bool is_final) const noexcept;

    Configuration configuration_;

    std::optional<TrajectoryWriter> trajectory_;
    std::optional<DiagnosticsLog> diagnostics_;

    std::filesystem::path checkpoint_path_;
    core::Index checkpoints_ = 0;
    bool first_ = true;
};

} // namespace orrery::sim
