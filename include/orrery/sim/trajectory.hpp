#pragma once

/// \file
/// The trajectory file: what the simulation looked like, frame by frame.
///
/// A run that keeps nothing produces one number at the end. A run that writes
/// every step as text produces a file larger than the machine's memory and takes
/// longer to write it than to compute it. This format is the middle: a fixed
/// header, then one frame per recorded step, each holding the positions and
/// optionally the velocities in the same component-array layout the solvers use.
/// `docs/formats/trajectory.md` is the specification.
///
/// Three decisions in it are worth stating here because they are what separate
/// it from a stream of `write(&struct)` calls.
///
/// **The masses are in the header, not in the frames.** They do not change
/// during a run, and a million-particle trajectory of a thousand frames would
/// otherwise carry eight gigabytes of a number that was already known. Having
/// them at all is what lets a reader compute an energy from a frame without the
/// configuration that produced it.
///
/// **There is no frame count.** The obvious header field cannot be written until
/// the run has finished, and a run that is killed by a full disc, a closed lid or
/// a scheduler is exactly the run whose output someone will want to look at. So
/// frames are self-delimiting and a reader consumes them until the file ends.
/// The cost is that a reader must scan to count them; the benefit is that a file
/// from an interrupted run is a valid file that stops early rather than a file
/// whose header describes frames that are not in it.
///
/// **Every frame carries its own checksum**, rather than the file carrying one.
/// That follows from the same argument: a whole-file checksum can only be
/// verified once the file is complete, so it would be absent from precisely the
/// files that most need checking. A per-frame checksum means a reader can accept
/// every frame that was written completely and reject a final one that was cut
/// in half, and can say which is which.
///
/// ## What this is not
///
/// A checkpoint. A trajectory frame is a record for a renderer or an analysis to
/// read, it may hold no velocities at all, and it holds no accelerations even
/// when it does, so a run cannot be resumed from one. `sim/checkpoint.hpp`
/// exists for that, and ADR-0033 records why the two are separate formats rather
/// than one format with the trajectory as a lossy setting of it.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_array.hpp"

namespace orrery::sim {

/// The eight bytes every trajectory file starts with.
///
/// A file that does not start with these is not one of these, which is the
/// first thing a reader checks and the reason a checkpoint pointed at a
/// trajectory reader is rejected in the first eight bytes rather than by
/// producing nonsense.
inline constexpr std::string_view kTrajectoryMagic = "ORRERYTJ";

/// The version of the layout in `docs/formats/trajectory.md`.
///
/// A reader refuses a version it does not know rather than guessing. There is
/// one version and this is it; the field exists so that a second one can be
/// added without the first becoming unreadable.
inline constexpr std::uint32_t kTrajectoryVersion = 1;

/// What a trajectory file's header says about it.
struct TrajectoryInfo {
    core::Index particle_count = 0;

    /// The timestep the run used, so that a frame's step index can be turned
    /// into a time without the configuration file.
    core::Real timestep = 0;

    bool has_velocities = false;

    /// Whether the file was written by a single-precision build.
    ///
    /// A reader compiled the other way round refuses the file. The alternative,
    /// converting, would mean a double-precision analysis silently reporting
    /// figures to sixteen digits of which seven were meaningful.
    bool single_precision = false;
};

/// One recorded instant.
struct TrajectoryFrame {
    /// Which step of the run this is, counting from zero at the initial state.
    core::Index step = 0;

    /// The simulated time, which is `step * timestep`.
    core::Real time = 0;

    core::Vec3Array positions;

    /// Empty unless the file has velocities in it.
    core::Vec3Array velocities;
};

/// Writes the format above.
///
/// Owns the file and closes it on destruction. A writer whose stream has failed
/// stays failed and reports it from `ok()`; the alternative, throwing from
/// inside the step loop, is forbidden by section 4 of the implementation plan
/// and would in any case turn a full disc into a lost simulation rather than a
/// lost output file.
class TrajectoryWriter {
public:
    /// Create the file and write its header.
    ///
    /// Throws `std::runtime_error` if the file cannot be opened, which is a
    /// setup boundary: a run should stop at once when its output path is
    /// unwritable rather than compute for an hour and then discover it.
    TrajectoryWriter(const std::filesystem::path& path, std::span<const core::Real> masses,
                     core::Real timestep, bool with_velocities);

    /// Append one frame.
    ///
    /// The configuration must have the same particle count the header declared.
    /// Does nothing once the stream has failed.
    void write_frame(core::Index step, core::Real time, const core::ParticleData& data);

    /// How many frames have been written.
    [[nodiscard]] core::Index frame_count() const noexcept { return frame_count_; }

    /// Whether every write so far reached the disc.
    [[nodiscard]] bool ok() const { return file_.good(); }

    /// Flush what has been written without closing the file.
    ///
    /// Called after a checkpoint is taken, so that the two outputs agree about
    /// how far the run had got if the process dies immediately afterwards.
    void flush() { file_.flush(); }

private:
    std::ofstream file_;
    core::Index particle_count_;
    bool has_velocities_;
    core::Index frame_count_ = 0;
};

/// Reads the format above.
///
/// The intended use is a loop over `read_frame` until it returns false, which
/// happens at a clean end of file. Anything else, a truncated frame or one whose
/// checksum does not match, throws instead, so that a reader cannot mistake a
/// damaged file for a short run.
class TrajectoryReader {
public:
    /// Open the file and read its header.
    ///
    /// Throws `std::runtime_error` if the file cannot be opened, does not carry
    /// the magic, is of an unknown version, or was written by a build of the
    /// other precision.
    explicit TrajectoryReader(const std::filesystem::path& path);

    [[nodiscard]] const TrajectoryInfo& info() const noexcept { return info_; }

    /// The masses from the header, in the order the frames use.
    [[nodiscard]] std::span<const core::Real> masses() const noexcept { return masses_; }

    /// Read the next frame into `frame`, or return false at the end of the file.
    ///
    /// `frame` is resized as needed and may be reused across calls, which is
    /// what keeps reading a long trajectory free of allocation after the first
    /// frame.
    ///
    /// Throws `std::runtime_error` on a partial or corrupt frame.
    [[nodiscard]] bool read_frame(TrajectoryFrame& frame);

private:
    std::ifstream file_;
    std::filesystem::path path_;
    TrajectoryInfo info_;
    std::vector<core::Real> masses_;
    core::Index frames_read_ = 0;
};

} // namespace orrery::sim
