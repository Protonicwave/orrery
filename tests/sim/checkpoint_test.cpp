#include "orrery/sim/checkpoint.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <stdexcept>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/sim/configuration.hpp"
#include "temporary_file.hpp"

namespace {

using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::Vec3;
using orrery::sim::Checkpoint;
using orrery::sim::SolverKind;
using orrery::sim::testing::TemporaryFile;

/// A state whose every array holds different values from every other.
///
/// Ten arrays are written and read in one fixed order, and a state whose
/// velocities matched its positions would pass a test in which the two had been
/// swapped.
[[nodiscard]] ParticleData varied_particles(Index count) {
    ParticleData data(count);
    const auto positions = data.positions();
    const auto velocities = data.velocities();
    const auto accelerations = data.accelerations();
    const auto masses = data.masses();

    for (Index index = 0; index < count; ++index) {
        const auto value = static_cast<Real>(index) / 7;
        positions.set(index, Vec3{value, value + 1, value + 2});
        velocities.set(index, Vec3{value + 3, value + 4, value + 5});
        accelerations.set(index, Vec3{value + 6, value + 7, value + 8});
        masses[index] = value + 9;
    }
    return data;
}

[[nodiscard]] Checkpoint sample_checkpoint(Index count) {
    Checkpoint checkpoint;
    checkpoint.step = 4321;
    checkpoint.particles = varied_particles(count);
    checkpoint.configuration.run.timestep = static_cast<Real>(0.001);
    checkpoint.configuration.run.steps = 10000;
    checkpoint.configuration.run.seed = 99;
    checkpoint.configuration.solver.kind = SolverKind::kBarnesHut;
    checkpoint.configuration.solver.softening = static_cast<Real>(0.05);
    checkpoint.configuration.initial_conditions.count = count;
    return checkpoint;
}

/// Whether two states agree in every bit of every component.
void require_identical(const ParticleData& left, const ParticleData& right) {
    REQUIRE(left.size() == right.size());

    const auto left_positions = left.positions();
    const auto right_positions = right.positions();
    const auto left_velocities = left.velocities();
    const auto right_velocities = right.velocities();
    const auto left_accelerations = left.accelerations();
    const auto right_accelerations = right.accelerations();

    for (Index index = 0; index < left.size(); ++index) {
        INFO("particle " << index);
        CHECK(left.masses()[index] == right.masses()[index]);
        CHECK(left_positions.x[index] == right_positions.x[index]);
        CHECK(left_positions.y[index] == right_positions.y[index]);
        CHECK(left_positions.z[index] == right_positions.z[index]);
        CHECK(left_velocities.x[index] == right_velocities.x[index]);
        CHECK(left_velocities.y[index] == right_velocities.y[index]);
        CHECK(left_velocities.z[index] == right_velocities.z[index]);
        CHECK(left_accelerations.x[index] == right_accelerations.x[index]);
        CHECK(left_accelerations.y[index] == right_accelerations.y[index]);
        CHECK(left_accelerations.z[index] == right_accelerations.z[index]);
    }
}

} // namespace

TEST_CASE("a checkpoint reads back the state it was given, exactly", "[sim][checkpoint]") {
    const TemporaryFile file("checkpoint-round-trip.ock");
    const Checkpoint original = sample_checkpoint(32);

    orrery::sim::write_checkpoint(file.path(), original);
    const Checkpoint read = orrery::sim::read_checkpoint(file.path());

    CHECK(read.step == original.step);
    require_identical(read.particles, original.particles);

    // The configuration travels with the state, which is what makes a
    // checkpoint enough on its own to resume from.
    CHECK(read.configuration == original.configuration);
}

TEST_CASE("the accelerations are stored rather than recomputed", "[sim][checkpoint]") {
    // ADR-0032. They could be derived from the positions for every solver in
    // this project, and storing them makes the resumed state a copy rather than
    // a reconstruction. The test is that values which no solver would ever
    // produce come back unchanged.
    const TemporaryFile file("checkpoint-accelerations.ock");

    Checkpoint original = sample_checkpoint(4);
    const auto accelerations = original.particles.accelerations();
    accelerations.set(0, Vec3{-1, -2, -3});
    accelerations.set(1, Vec3{static_cast<Real>(1e30), static_cast<Real>(-1e30), 0});

    orrery::sim::write_checkpoint(file.path(), original);
    const Checkpoint read = orrery::sim::read_checkpoint(file.path());

    const auto read_accelerations = read.particles.accelerations();
    CHECK(read_accelerations.get(0).x == static_cast<Real>(-1));
    CHECK(read_accelerations.get(1).x == static_cast<Real>(1e30));
    CHECK(read_accelerations.get(1).z == static_cast<Real>(0));
}

TEST_CASE("the awkward floating-point values survive", "[sim][checkpoint]") {
    // The bit patterns a text format loses. A negative zero that came back as a
    // positive one would change the sign of a velocity that had been brought
    // exactly to rest, and an infinity or a NaN in a checkpoint is a run that
    // has gone wrong in a way its author needs to be able to see rather than
    // one the format should quietly repair.
    const TemporaryFile file("checkpoint-awkward.ock");

    Checkpoint original = sample_checkpoint(4);
    const auto velocities = original.particles.velocities();
    velocities.set(0, Vec3{static_cast<Real>(-0.0), std::numeric_limits<Real>::infinity(),
                           -std::numeric_limits<Real>::infinity()});
    velocities.set(1,
                   Vec3{std::numeric_limits<Real>::quiet_NaN(),
                        std::numeric_limits<Real>::denorm_min(), std::numeric_limits<Real>::max()});

    orrery::sim::write_checkpoint(file.path(), original);
    const Checkpoint read = orrery::sim::read_checkpoint(file.path());

    const auto read_velocities = read.particles.velocities();
    CHECK(std::signbit(read_velocities.get(0).x));
    CHECK(read_velocities.get(0).x == static_cast<Real>(0));
    CHECK(std::isinf(read_velocities.get(0).y));
    CHECK(read_velocities.get(0).y > 0);
    CHECK(std::isinf(read_velocities.get(0).z));
    CHECK(read_velocities.get(0).z < 0);
    CHECK(std::isnan(read_velocities.get(1).x));
    CHECK(read_velocities.get(1).y == std::numeric_limits<Real>::denorm_min());
    CHECK(read_velocities.get(1).z == std::numeric_limits<Real>::max());
}

TEST_CASE("writing a checkpoint leaves no temporary behind", "[sim][checkpoint]") {
    const TemporaryFile file("checkpoint-atomic.ock");
    orrery::sim::write_checkpoint(file.path(), sample_checkpoint(8));

    std::filesystem::path partial = file.path();
    partial += ".partial";
    CHECK_FALSE(std::filesystem::exists(partial));
    CHECK(std::filesystem::exists(file.path()));
}

TEST_CASE("a second checkpoint replaces the first", "[sim][checkpoint]") {
    // The path is one file overwritten rather than a pattern, so that a long run
    // does not fill a disc with states nobody asked for. The state a person
    // wants after an interruption is the last one.
    const TemporaryFile file("checkpoint-replace.ock");

    Checkpoint first = sample_checkpoint(8);
    first.step = 100;
    orrery::sim::write_checkpoint(file.path(), first);

    Checkpoint second = sample_checkpoint(8);
    second.step = 200;
    orrery::sim::write_checkpoint(file.path(), second);

    CHECK(orrery::sim::read_checkpoint(file.path()).step == 200);
}

TEST_CASE("a damaged checkpoint is refused rather than resumed from", "[sim][checkpoint]") {
    // Continuing from a state that might be wrong would produce results
    // indistinguishable from correct ones, which is the worst kind of failure
    // this project can have.
    const TemporaryFile file("checkpoint-damaged.ock");
    orrery::sim::write_checkpoint(file.path(), sample_checkpoint(16));

    SECTION("a byte changed in the middle") {
        std::fstream stream(file.path(), std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(stream);
        const auto size = static_cast<std::streamoff>(std::filesystem::file_size(file.path()));
        stream.seekp(size / 2);
        const char rubbish = '\x5a';
        stream.write(&rubbish, 1);
        stream.close();

        CHECK_THROWS_AS(orrery::sim::read_checkpoint(file.path()), std::runtime_error);
    }

    SECTION("a file cut short, as an interrupted write would leave") {
        const auto size = std::filesystem::file_size(file.path());
        std::filesystem::resize_file(file.path(), size / 2);

        CHECK_THROWS_AS(orrery::sim::read_checkpoint(file.path()), std::runtime_error);
    }
}

TEST_CASE("a file that is not a checkpoint is refused", "[sim][checkpoint]") {
    const TemporaryFile file("checkpoint-not-one.ock");
    {
        std::ofstream stream(file.path(), std::ios::binary);
        stream << "ORRERYTJ and then some bytes that are not a checkpoint";
    }

    // Including a trajectory, which starts with eight bytes of a different
    // magic and would otherwise be read as a state.
    CHECK_THROWS_AS(orrery::sim::read_checkpoint(file.path()), std::runtime_error);
}

TEST_CASE("an absent checkpoint is an error", "[sim][checkpoint]") {
    CHECK_THROWS_AS(orrery::sim::read_checkpoint("no-such-checkpoint.ock"), std::runtime_error);
}

TEST_CASE("a checkpoint of no particles round-trips", "[sim][checkpoint]") {
    const TemporaryFile file("checkpoint-empty.ock");

    Checkpoint original;
    original.step = 0;
    orrery::sim::write_checkpoint(file.path(), original);

    const Checkpoint read = orrery::sim::read_checkpoint(file.path());
    CHECK(read.particles.empty());
    CHECK(read.configuration == original.configuration);
}
