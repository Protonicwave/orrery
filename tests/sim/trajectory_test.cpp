#include "orrery/sim/trajectory.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "temporary_file.hpp"

namespace {

using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::Vec3;
using orrery::sim::TrajectoryFrame;
using orrery::sim::TrajectoryReader;
using orrery::sim::TrajectoryWriter;

using orrery::sim::testing::TemporaryFile;

/// Particles whose components are all different from each other.
///
/// A frame written from a configuration where the x, y and z arrays held the
/// same values would pass a test that had them the wrong way round, and the
/// component-array layout makes that exactly the mistake a reader or writer
/// could make.
[[nodiscard]] ParticleData distinguishable_particles(Index count) {
    ParticleData data(count);
    const auto positions = data.positions();
    const auto velocities = data.velocities();
    const auto masses = data.masses();

    for (Index index = 0; index < count; ++index) {
        const auto value = static_cast<Real>(index);
        positions.set(index, Vec3{value, value + 100, value + 200});
        velocities.set(index, Vec3{value + 300, value + 400, value + 500});
        masses[index] = value + 1;
    }
    return data;
}

} // namespace

TEST_CASE("a trajectory reads back exactly what was written", "[sim][trajectory]") {
    const TemporaryFile file("trajectory-round-trip.otj");
    const ParticleData data = distinguishable_particles(8);

    {
        TrajectoryWriter writer(file.path(), data.masses(), static_cast<Real>(0.25), false);
        writer.write_frame(0, 0, data);
        writer.write_frame(10, static_cast<Real>(2.5), data);
        CHECK(writer.frame_count() == 2);
        CHECK(writer.ok());
    }

    TrajectoryReader reader(file.path());
    CHECK(reader.info().particle_count == 8);
    CHECK(reader.info().timestep == static_cast<Real>(0.25));
    CHECK_FALSE(reader.info().has_velocities);
    CHECK(reader.info().single_precision == orrery::core::kSinglePrecision);

    // The masses live in the header rather than in every frame, since they do
    // not change during a run.
    REQUIRE(reader.masses().size() == 8);
    for (Index index = 0; index < 8; ++index) {
        CHECK(reader.masses()[index] == data.masses()[index]);
    }

    TrajectoryFrame frame;
    REQUIRE(reader.read_frame(frame));
    CHECK(frame.step == 0);
    CHECK(frame.time == static_cast<Real>(0));

    const auto positions = data.positions();
    const auto read = frame.positions.view();
    REQUIRE(read.size() == 8);
    for (Index index = 0; index < 8; ++index) {
        INFO("particle " << index);
        // Equality rather than a tolerance. The format stores the bit pattern,
        // so anything but exact agreement is a defect in the format rather than
        // rounding.
        CHECK(read.x[index] == positions.x[index]);
        CHECK(read.y[index] == positions.y[index]);
        CHECK(read.z[index] == positions.z[index]);
    }

    REQUIRE(reader.read_frame(frame));
    CHECK(frame.step == 10);
    CHECK(frame.time == static_cast<Real>(2.5));

    // A clean end of file, which is how a reader knows the run finished rather
    // than that the file was cut short.
    CHECK_FALSE(reader.read_frame(frame));
}

TEST_CASE("velocities are written when they are asked for", "[sim][trajectory]") {
    const TemporaryFile file("trajectory-velocities.otj");
    const ParticleData data = distinguishable_particles(4);

    {
        TrajectoryWriter writer(file.path(), data.masses(), 1, true);
        writer.write_frame(0, 0, data);
    }

    TrajectoryReader reader(file.path());
    CHECK(reader.info().has_velocities);

    TrajectoryFrame frame;
    REQUIRE(reader.read_frame(frame));

    const auto expected = data.velocities();
    const auto read = frame.velocities.view();
    REQUIRE(read.size() == 4);
    for (Index index = 0; index < 4; ++index) {
        CHECK(read.x[index] == expected.x[index]);
        CHECK(read.y[index] == expected.y[index]);
        CHECK(read.z[index] == expected.z[index]);
    }
}

TEST_CASE("a run that was killed leaves a file that stops early", "[sim][trajectory]") {
    // The reason the format has no frame count in its header and a checksum per
    // frame rather than per file. A run killed by a full disc or a closed lid is
    // exactly the run whose output someone wants to look at, and every complete
    // frame in it should still be readable.
    const TemporaryFile file("trajectory-truncated.otj");
    const ParticleData data = distinguishable_particles(16);

    {
        TrajectoryWriter writer(file.path(), data.masses(), 1, false);
        writer.write_frame(0, 0, data);
        writer.write_frame(1, 1, data);
        writer.write_frame(2, 2, data);
    }

    const auto full_size = static_cast<std::uintmax_t>(std::filesystem::file_size(file.path()));

    // Cut off most of the last frame, which is what a process dying mid-write
    // produces.
    std::filesystem::resize_file(file.path(), full_size - 100);

    TrajectoryReader reader(file.path());
    TrajectoryFrame frame;
    CHECK(reader.read_frame(frame));
    CHECK(reader.read_frame(frame));

    // The third frame is present but incomplete, and that is an error rather
    // than a quiet end: a reader must not mistake a damaged file for a short
    // run.
    CHECK_THROWS_AS(reader.read_frame(frame), std::runtime_error);
}

TEST_CASE("a frame that was damaged in place is caught", "[sim][trajectory]") {
    // Truncation is caught by the length alone. This is the case the checksum is
    // for: a file of exactly the right length with a byte changed in the middle
    // of it.
    const TemporaryFile file("trajectory-corrupt.otj");
    const ParticleData data = distinguishable_particles(16);

    {
        TrajectoryWriter writer(file.path(), data.masses(), 1, false);
        writer.write_frame(0, 0, data);
    }

    {
        std::fstream file_stream(file.path(), std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file_stream);
        const auto size = static_cast<std::streamoff>(std::filesystem::file_size(file.path()));
        file_stream.seekp(size - 40);
        const char rubbish = '\x7f';
        file_stream.write(&rubbish, 1);
    }

    TrajectoryReader reader(file.path());
    TrajectoryFrame frame;
    CHECK_THROWS_AS(reader.read_frame(frame), std::runtime_error);
}

TEST_CASE("a file that is not a trajectory is refused at once", "[sim][trajectory]") {
    const TemporaryFile file("trajectory-not-one.otj");
    {
        std::ofstream stream(file.path(), std::ios::binary);
        stream << "this is not a trajectory, it is a sentence";
    }

    CHECK_THROWS_AS(TrajectoryReader(file.path()), std::runtime_error);
}

TEST_CASE("an absent trajectory is an error rather than an empty one", "[sim][trajectory]") {
    CHECK_THROWS_AS(TrajectoryReader("no-such-trajectory.otj"), std::runtime_error);
}

TEST_CASE("a frame of the wrong length is refused rather than written", "[sim][trajectory]") {
    // Writing it would make every frame after it unreadable, since the format
    // has no per-frame length and a reader advances by the header's count.
    const TemporaryFile file("trajectory-wrong-length.otj");
    const ParticleData data = distinguishable_particles(8);
    const ParticleData other = distinguishable_particles(9);

    {
        TrajectoryWriter writer(file.path(), data.masses(), 1, false);
        writer.write_frame(0, 0, other);
        CHECK(writer.frame_count() == 0);

        writer.write_frame(0, 0, data);
        CHECK(writer.frame_count() == 1);
    }

    TrajectoryReader reader(file.path());
    TrajectoryFrame frame;
    CHECK(reader.read_frame(frame));
    CHECK_FALSE(reader.read_frame(frame));
}

TEST_CASE("a trajectory of no particles is still a trajectory", "[sim][trajectory]") {
    const TemporaryFile file("trajectory-empty.otj");
    const ParticleData data;

    {
        TrajectoryWriter writer(file.path(), data.masses(), 1, false);
        writer.write_frame(0, 0, data);
    }

    TrajectoryReader reader(file.path());
    CHECK(reader.info().particle_count == 0);

    TrajectoryFrame frame;
    CHECK(reader.read_frame(frame));
    CHECK_FALSE(reader.read_frame(frame));
}
