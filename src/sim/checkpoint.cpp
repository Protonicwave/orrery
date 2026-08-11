#include "orrery/sim/checkpoint.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/sim/binary_stream.hpp"
#include "orrery/sim/config_file.hpp"

namespace orrery::sim {
namespace {

constexpr std::uint32_t kFlagSinglePrecision = 1U << 0U;
constexpr std::uint32_t kKnownFlags = kFlagSinglePrecision;

/// The same bound the trajectory reader applies, and for the same reason: a
/// header claiming an absurd particle count should be refused before it is
/// used to size an allocation.
constexpr core::Index kParticleCountLimit = 1ULL << 40U;

/// The most configuration text a checkpoint may carry.
///
/// The format writes about a kilobyte. A megabyte is a thousand times that and
/// still small enough to read without thinking about it, so a file claiming
/// more has been damaged rather than written by a version with more settings.
constexpr std::size_t kConfigurationLimit = 1U << 20U;

[[noreturn]] void fail(const std::filesystem::path& path, const std::string& message) {
    throw std::runtime_error(path.string() + ": " + message);
}

/// Write every component array of the state, in one fixed order.
///
/// Masses first, then the three vector quantities in the order the particle
/// store lists them. The order is arbitrary and only has to match the reader,
/// which is why both are in this file and neither is written twice.
void write_state(BinaryWriter& writer, const core::ParticleData& particles) {
    writer.write_reals(particles.masses());

    const auto positions = particles.positions();
    writer.write_reals(positions.x);
    writer.write_reals(positions.y);
    writer.write_reals(positions.z);

    const auto velocities = particles.velocities();
    writer.write_reals(velocities.x);
    writer.write_reals(velocities.y);
    writer.write_reals(velocities.z);

    const auto accelerations = particles.accelerations();
    writer.write_reals(accelerations.x);
    writer.write_reals(accelerations.y);
    writer.write_reals(accelerations.z);
}

void read_state(BinaryReader& reader, core::ParticleData& particles) {
    reader.read_reals(particles.masses());

    const auto positions = particles.positions();
    reader.read_reals(positions.x);
    reader.read_reals(positions.y);
    reader.read_reals(positions.z);

    const auto velocities = particles.velocities();
    reader.read_reals(velocities.x);
    reader.read_reals(velocities.y);
    reader.read_reals(velocities.z);

    const auto accelerations = particles.accelerations();
    reader.read_reals(accelerations.x);
    reader.read_reals(accelerations.y);
    reader.read_reals(accelerations.z);
}

} // namespace

void write_checkpoint(const std::filesystem::path& path, const Checkpoint& checkpoint) {
    // A neighbour of the target rather than a system temporary directory,
    // because a rename is only guaranteed to be atomic within one filesystem and
    // the temporary directory is frequently on another one. On the same
    // directory it is a directory entry being repointed.
    std::filesystem::path partial = path;
    partial += ".partial";

    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        if (!file) {
            fail(partial, "could not be opened for writing");
        }

        std::ostringstream configuration;
        write_configuration(configuration, checkpoint.configuration);

        std::uint32_t flags = 0;
        if constexpr (core::kSinglePrecision) {
            flags |= kFlagSinglePrecision;
        }

        BinaryWriter writer(file);
        writer.write_bytes(kCheckpointMagic);
        writer.write_u32(kCheckpointVersion);
        writer.write_u32(flags);
        writer.write_u64(checkpoint.step);
        writer.write_u64(checkpoint.particles.size());
        writer.write_string(configuration.str());
        write_state(writer, checkpoint.particles);
        writer.write_u64(writer.checksum());

        // Checked before the rename, so that a write which failed leaves the
        // previous checkpoint in place. Closing is what flushes, so the check
        // has to happen after the stream leaves scope or be forced here.
        file.flush();
        if (!file) {
            fail(partial, "could not be written");
        }
    }

    std::error_code error;
    std::filesystem::rename(partial, path, error);
    if (error) {
        fail(path, "could not be replaced with the new checkpoint: " + error.message());
    }
}

Checkpoint read_checkpoint(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail(path, "could not be opened for reading");
    }

    BinaryReader reader(file);
    if (reader.read_bytes(kCheckpointMagic.size()) != kCheckpointMagic) {
        fail(path, "is not an Orrery checkpoint");
    }

    const std::uint32_t version = reader.read_u32();
    if (version != kCheckpointVersion) {
        fail(path, "is version " + std::to_string(version) + ", and this build reads version " +
                       std::to_string(kCheckpointVersion));
    }

    const std::uint32_t flags = reader.read_u32();
    if ((flags & ~kKnownFlags) != 0) {
        fail(path, "sets header flags this build does not know about");
    }
    if (((flags & kFlagSinglePrecision) != 0) != core::kSinglePrecision) {
        fail(path, core::kSinglePrecision
                       ? "was written in double precision and this is a single-precision build"
                       : "was written in single precision and this is a double-precision build");
    }

    Checkpoint checkpoint;
    checkpoint.step = static_cast<core::Index>(reader.read_u64());

    const std::uint64_t count = reader.read_u64();
    if (!reader.ok() || count > kParticleCountLimit) {
        fail(path, "has an unreadable header");
    }

    const std::string text = reader.read_string(kConfigurationLimit);
    if (!reader.ok()) {
        fail(path, "has an unreadable configuration in it");
    }

    // Parsed rather than stored field by field, so that the checkpoint carries
    // the configuration in the one format this project already specifies and
    // tests. A checkpoint whose configuration cannot be parsed is damaged, and
    // the parser's own message names the line.
    std::istringstream configuration(text);
    checkpoint.configuration = parse_configuration(configuration, path.string());

    checkpoint.particles.resize(static_cast<core::Index>(count));
    read_state(reader, checkpoint.particles);

    const std::uint64_t expected = reader.checksum();
    const std::uint64_t stored = reader.read_u64();
    if (!reader.ok()) {
        fail(path, "ends before its state is complete");
    }
    if (stored != expected) {
        fail(path, "does not match its checksum, so it was damaged or written by an interrupted "
                   "run");
    }

    return checkpoint;
}

} // namespace orrery::sim
