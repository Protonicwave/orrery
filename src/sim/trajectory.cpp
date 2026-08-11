#include "orrery/sim/trajectory.hpp"

#include <cstdint>
#include <filesystem>
#include <ios>
#include <span>
#include <stdexcept>
#include <string>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/sim/binary_stream.hpp"

namespace orrery::sim {
namespace {

/// The header flags. Two bits used, the rest reserved and required to be zero,
/// so that a file written by a later version that sets one is refused here
/// rather than read as though the bit meant nothing.
constexpr std::uint32_t kFlagSinglePrecision = 1U << 0U;
constexpr std::uint32_t kFlagHasVelocities = 1U << 1U;
constexpr std::uint32_t kKnownFlags = kFlagSinglePrecision | kFlagHasVelocities;

/// The largest particle count a reader will believe from a header.
///
/// A corrupt or hostile file whose count field says 2^64 - 1 would otherwise
/// have the reader ask for a hundred exabytes of masses before it discovered
/// the file was eighty bytes long. The bound is far above the two million
/// particles this machine can actually integrate, so it constrains nothing a
/// real run produces and stops the allocation a nonsense header would ask for.
constexpr core::Index kParticleCountLimit = 1ULL << 40U;

[[noreturn]] void fail(const std::filesystem::path& path, const std::string& message) {
    throw std::runtime_error(path.string() + ": " + message);
}

} // namespace

TrajectoryWriter::TrajectoryWriter(const std::filesystem::path& path,
                                   std::span<const core::Real> masses, core::Real timestep,
                                   bool with_velocities)
    : file_(path, std::ios::binary),
      particle_count_(masses.size()),
      has_velocities_(with_velocities) {
    if (!file_) {
        fail(path, "could not be opened for writing");
    }

    std::uint32_t flags = 0;
    if constexpr (core::kSinglePrecision) {
        flags |= kFlagSinglePrecision;
    }
    if (with_velocities) {
        flags |= kFlagHasVelocities;
    }

    BinaryWriter writer(file_);
    writer.write_bytes(kTrajectoryMagic);
    writer.write_u32(kTrajectoryVersion);
    writer.write_u32(flags);
    writer.write_u64(particle_count_);
    writer.write_real(timestep);
    writer.write_reals(masses);
    writer.write_u64(writer.checksum());

    if (!file_) {
        fail(path, "could not be written");
    }
}

void TrajectoryWriter::write_frame(core::Index step, core::Real time,
                                   const core::ParticleData& data) {
    if (!file_.good() || data.size() != particle_count_) {
        // A frame of the wrong length would make every frame after it
        // unreadable, since the format has no per-frame length field and a
        // reader advances by the count the header declared. Refusing to write it
        // keeps the file readable up to the point the mistake was made.
        return;
    }

    // A writer of its own per frame, because the checksum at the end of a frame
    // covers that frame and nothing else. That is what lets a reader accept
    // every complete frame of a file whose last one was cut short.
    BinaryWriter writer(file_);
    writer.write_u64(step);
    writer.write_real(time);

    const auto positions = data.positions();
    writer.write_reals(positions.x);
    writer.write_reals(positions.y);
    writer.write_reals(positions.z);

    if (has_velocities_) {
        const auto velocities = data.velocities();
        writer.write_reals(velocities.x);
        writer.write_reals(velocities.y);
        writer.write_reals(velocities.z);
    }

    writer.write_u64(writer.checksum());
    ++frame_count_;
}

TrajectoryReader::TrajectoryReader(const std::filesystem::path& path)
    : file_(path, std::ios::binary), path_(path) {
    if (!file_) {
        fail(path_, "could not be opened for reading");
    }

    BinaryReader reader(file_);
    if (reader.read_bytes(kTrajectoryMagic.size()) != kTrajectoryMagic) {
        fail(path_, "is not an Orrery trajectory file");
    }

    const std::uint32_t version = reader.read_u32();
    if (version != kTrajectoryVersion) {
        fail(path_, "is version " + std::to_string(version) + ", and this build reads version " +
                        std::to_string(kTrajectoryVersion));
    }

    const std::uint32_t flags = reader.read_u32();
    if ((flags & ~kKnownFlags) != 0) {
        fail(path_, "sets header flags this build does not know about");
    }

    info_.single_precision = (flags & kFlagSinglePrecision) != 0;
    info_.has_velocities = (flags & kFlagHasVelocities) != 0;
    if (info_.single_precision != core::kSinglePrecision) {
        // Refused rather than converted. The two builds disagree about how many
        // bytes a Real occupies, so reading one as the other would not merely
        // lose digits, it would read the file at the wrong stride and produce
        // positions that are half of one coordinate and half of the next.
        fail(path_, info_.single_precision
                        ? "was written in single precision and this is a double-precision build"
                        : "was written in double precision and this is a single-precision build");
    }

    const std::uint64_t count = reader.read_u64();
    if (!reader.ok() || count > kParticleCountLimit) {
        fail(path_, "has an unreadable header");
    }
    info_.particle_count = static_cast<core::Index>(count);
    info_.timestep = reader.read_real();

    masses_.resize(info_.particle_count);
    reader.read_reals(masses_);

    const std::uint64_t expected = reader.checksum();
    const std::uint64_t stored = reader.read_u64();
    if (!reader.ok()) {
        fail(path_, "ends inside its header");
    }
    if (stored != expected) {
        fail(path_, "has a header that does not match its checksum");
    }
}

bool TrajectoryReader::read_frame(TrajectoryFrame& frame) {
    // Distinguishing the end of the file from a truncated frame needs one look
    // ahead: at a clean end there is nothing at all, and anything else is the
    // start of a frame that has to be complete.
    if (file_.peek() == std::char_traits<char>::eof()) {
        return false;
    }

    BinaryReader reader(file_);
    frame.step = static_cast<core::Index>(reader.read_u64());
    frame.time = reader.read_real();

    frame.positions.resize(info_.particle_count);
    const auto positions = frame.positions.view();
    reader.read_reals(positions.x);
    reader.read_reals(positions.y);
    reader.read_reals(positions.z);

    if (info_.has_velocities) {
        frame.velocities.resize(info_.particle_count);
        const auto velocities = frame.velocities.view();
        reader.read_reals(velocities.x);
        reader.read_reals(velocities.y);
        reader.read_reals(velocities.z);
    }

    const std::uint64_t expected = reader.checksum();
    const std::uint64_t stored = reader.read_u64();
    if (!reader.ok()) {
        fail(path_, "ends inside frame " + std::to_string(frames_read_));
    }
    if (stored != expected) {
        fail(path_, "frame " + std::to_string(frames_read_) + " does not match its checksum");
    }

    ++frames_read_;
    return true;
}

} // namespace orrery::sim
