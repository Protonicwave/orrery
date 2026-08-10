#pragma once

/// \file
/// The particle store: one contiguous array per component.
///
/// This is the layout decision the rest of the project is built on, and it is
/// made for bandwidth rather than for tidiness. The direct force kernel reads
/// positions and masses and writes accelerations. It never reads velocities.
///
/// Held as an array of particle structs, a position, a velocity, an
/// acceleration and a mass occupy 80 bytes in double precision, so a 64-byte
/// cache line carries fewer than one particle and most of every line fetched is
/// velocity and acceleration data the kernel does not use. Held as separate
/// component arrays, every byte of every line the kernel touches is a
/// coordinate or a mass it is about to multiply. On a machine where the binding
/// constraint is roughly 135 GB/s of memory bandwidth shared with the
/// integrated GPU, that is the difference between a kernel that is limited by
/// arithmetic and one that is limited by waiting.
///
/// Separating the components rather than only the quantities buys the second
/// half of the argument: the x coordinates of consecutive particles are
/// adjacent, so a vector load takes eight of them with one instruction. An
/// array of `Vec3` would need a strided gather instead. ADR-0004 records both
/// choices and what they cost.

#include <span>
#include <vector>

#include "orrery/core/aligned_allocator.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::core {

/// Storage for a set of point masses in structure-of-arrays layout.
///
/// The class invariant is that all ten component arrays hold exactly `size()`
/// elements. Every operation that can change a length goes through one private
/// helper that applies it to all ten, so an array cannot be forgotten, and an
/// eleventh added later is caught at the one place that lists them rather than
/// at each of the four operations that resize them.
class ParticleData {
public:
    ParticleData() = default;

    /// Create `count` particles with every component zero.
    ///
    /// Zero rather than uninitialised, and deliberately so: a particle of zero
    /// mass at the origin with no velocity exerts no force and feels the
    /// forces it is given, so a container that has been sized but not yet
    /// filled by an initial-condition generator perturbs nothing. Reading
    /// uninitialised storage would instead make a partially filled container
    /// produce results that vary between runs.
    explicit ParticleData(Index count);

    [[nodiscard]] Index size() const noexcept { return mass_.size(); }

    [[nodiscard]] bool empty() const noexcept { return mass_.empty(); }

    /// The number of particles the arrays can hold without reallocating.
    ///
    /// Reported from one array because the ten are grown together and always
    /// agree, for the same reason their lengths do.
    [[nodiscard]] Index capacity() const noexcept { return mass_.capacity(); }

    /// Make room for `count` particles without changing how many there are.
    void reserve(Index count);

    /// Change the number of particles, zero-filling any that are new.
    void resize(Index count);

    /// Remove every particle, keeping the storage for reuse.
    void clear() noexcept;

    /// Append one particle with zero acceleration and return its index.
    ///
    /// For setup code and tests. Initial-condition generators that know their
    /// particle count size the container once and write through the views
    /// instead, which is both faster and how the solvers see the data.
    Index add(Vec3 position, Vec3 velocity, Real mass);

    [[nodiscard]] Vec3Span<Real> positions() noexcept {
        return {position_x_, position_y_, position_z_};
    }

    [[nodiscard]] Vec3Span<const Real> positions() const noexcept {
        return {position_x_, position_y_, position_z_};
    }

    [[nodiscard]] Vec3Span<Real> velocities() noexcept {
        return {velocity_x_, velocity_y_, velocity_z_};
    }

    [[nodiscard]] Vec3Span<const Real> velocities() const noexcept {
        return {velocity_x_, velocity_y_, velocity_z_};
    }

    [[nodiscard]] Vec3Span<Real> accelerations() noexcept {
        return {acceleration_x_, acceleration_y_, acceleration_z_};
    }

    [[nodiscard]] Vec3Span<const Real> accelerations() const noexcept {
        return {acceleration_x_, acceleration_y_, acceleration_z_};
    }

    [[nodiscard]] std::span<Real> masses() noexcept { return mass_; }

    [[nodiscard]] std::span<const Real> masses() const noexcept { return mass_; }

private:
    using ComponentArray = std::vector<Real, AlignedAllocator<Real>>;

    /// The one place in the project that lists the component arrays.
    ///
    /// Every length-changing operation is expressed through this, so the size
    /// invariant holds by construction rather than by four separate
    /// implementations remembering the same ten names.
    template<typename Operation> void for_each_array(Operation operation) {
        operation(position_x_);
        operation(position_y_);
        operation(position_z_);
        operation(velocity_x_);
        operation(velocity_y_);
        operation(velocity_z_);
        operation(acceleration_x_);
        operation(acceleration_y_);
        operation(acceleration_z_);
        operation(mass_);
    }

    /// The first allocation `add` asks for, in particles.
    ///
    /// Eight doubles fill exactly one 64-byte cache line, so the smallest
    /// container this project allocates is also the smallest one whose arrays
    /// occupy whole lines.
    static constexpr Index kInitialCapacity = 8;

    ComponentArray position_x_;
    ComponentArray position_y_;
    ComponentArray position_z_;
    ComponentArray velocity_x_;
    ComponentArray velocity_y_;
    ComponentArray velocity_z_;
    ComponentArray acceleration_x_;
    ComponentArray acceleration_y_;
    ComponentArray acceleration_z_;
    ComponentArray mass_;
};

} // namespace orrery::core
