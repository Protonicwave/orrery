#pragma once

/// \file
/// The complete state of a run, written so that it can be picked up exactly
/// where it was put down.
///
/// The requirement in section 7 of the implementation plan is precise and worth
/// quoting: a long run can be interrupted and resumed to bitwise-identical
/// state. Not nearly identical, and not identical to the precision anyone is
/// likely to look at. That is a stronger requirement than it sounds, and it is
/// what decides everything in this file.
///
/// **Every number is stored as its exact bit pattern**, through
/// `sim/binary_stream.hpp`. A checkpoint in text would need seventeen
/// significant digits per component to round-trip a double, would be four times
/// the size, and would still be wrong for a NaN or a negative zero.
///
/// **The accelerations are stored**, though they could be recomputed from the
/// positions. The integrators of Phase 4 carry the acceleration between steps as
/// a documented invariant (ADR-0013), so a resumed run has to start with the one
/// its predecessor ended with. Recomputing it would give the same answer for
/// every solver in this project, since all of them are deterministic functions
/// of the positions and masses, and that is exactly the sort of property that is
/// true until someone adds a solver for which it is not. Storing three more
/// arrays makes the resumed state a copy rather than a reconstruction, and
/// ADR-0032 records the trade.
///
/// **The configuration is stored inside the checkpoint**, as the text of the
/// format in `sim/config_file.hpp`. A checkpoint is then sufficient on its own:
/// resuming needs the file and nothing else, and a run cannot be resumed under
/// settings that differ from the ones it was taken under, which is the failure
/// this removes rather than diagnoses. It costs about a kilobyte against a state
/// that is eighty bytes a particle.
///
/// **The file is written atomically**, to a neighbouring temporary name that is
/// then renamed over the target. A checkpoint exists to survive a run being
/// killed, and the moment a run is most likely to be killed is not chosen to
/// avoid the moment the checkpoint is half written. Without the rename, the
/// signal that arrives during the write destroys the previous checkpoint as well
/// as the current one, which turns the feature into a liability.

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/sim/configuration.hpp"

namespace orrery::sim {

/// The eight bytes every checkpoint starts with.
inline constexpr std::string_view kCheckpointMagic = "ORRERYCK";

/// The version of the layout in `docs/formats/checkpoint.md`.
inline constexpr std::uint32_t kCheckpointVersion = 1;

/// A run, stopped.
struct Checkpoint {
    /// The settings the run was started with, including the ones the command
    /// line supplied rather than the file.
    Configuration configuration;

    /// How many steps had been taken when this was written.
    ///
    /// The simulated time is this times the timestep, and is not stored
    /// separately. Two fields that must agree are how they come to disagree,
    /// and this is the one the run counts in.
    core::Index step = 0;

    /// Positions, velocities, accelerations and masses, exactly as they were.
    core::ParticleData particles;
};

/// Write `checkpoint` to `path`, atomically.
///
/// The bytes go to a temporary file beside the target, which is then renamed
/// over it. On every platform this project builds for, that rename either
/// happens or does not, so a reader sees the previous checkpoint or the new one
/// and never half of either.
///
/// Throws `std::runtime_error` if the file cannot be written or the rename
/// fails. Called from the output layer between steps rather than from inside
/// one, so an exception here does not cross a kernel.
void write_checkpoint(const std::filesystem::path& path, const Checkpoint& checkpoint);

/// Read a checkpoint back.
///
/// Throws `std::runtime_error` if the file cannot be opened, does not carry the
/// magic, is of an unknown version, was written by a build of the other
/// precision, or does not match its checksum. A checkpoint that fails any of
/// those is not resumed from under a warning: continuing from a state that might
/// be damaged would produce results indistinguishable from correct ones.
[[nodiscard]] Checkpoint read_checkpoint(const std::filesystem::path& path);

} // namespace orrery::sim
