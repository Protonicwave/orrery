#pragma once

/// \file
/// What an integrator is allowed to know about gravity.
///
/// An integrator advances a state under an acceleration that depends on
/// position. It does not care whether that acceleration comes from direct
/// summation, from a tree walk, from the GPU, or from an analytic test field,
/// and it must not, because the solvers layer sits above this one and a header
/// here may not reach up into it. This interface is the whole of the contract
/// between the two, and the solvers of Phase 5 and Phase 8 implement it.
///
/// The signature takes spans rather than a `ParticleData` for one reason, and it
/// is the reason a non-symplectic method exists in this project at all: RK4
/// evaluates the acceleration at stage positions that are not the state of any
/// particle. Handing the evaluator a particle store would force those stage
/// positions to be written into one first, which means either a second store
/// held for the purpose or a temporary swap of the real one. Spans let the
/// caller point the evaluator at whatever positions it currently holds.
///
/// ADR-0012 records the alternatives, the function-template form among them.

#include <span>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::integrators {

/// The acceleration produced by a set of masses at a set of positions.
///
/// One virtual call per force evaluation, which is between one and four per
/// timestep, ahead of the N^2 or N log N interactions the implementation is
/// about to compute. That is the boundary the implementation plan permits
/// virtual dispatch at, and no call through this interface appears inside a loop
/// over particles.
class AccelerationField {
public:
    virtual ~AccelerationField() = default;

    /// Write the acceleration of each position into `accelerations`.
    ///
    /// All three views describe the same particles in the same order and have
    /// the same length. The masses belong to the particles at those positions,
    /// which matters when the positions are a Runge-Kutta stage rather than the
    /// state: a stage moves the particles without changing what they are.
    ///
    /// The accelerations are written rather than accumulated, so the caller
    /// passes a buffer without clearing it first.
    ///
    /// Not `const`, because an implementation legitimately has state to change:
    /// the interaction counter the direct solver keeps for cross-algorithm
    /// comparison, and the tree the Barnes-Hut solver rebuilds as the particles
    /// move. A const interface would push both into `mutable` members, which
    /// says the same thing less honestly.
    virtual void evaluate(core::Vec3Span<const core::Real> positions,
                          std::span<const core::Real> masses,
                          core::Vec3Span<core::Real> accelerations) = 0;

protected:
    // Copying and moving are the derived class's business. They are declared
    // here because a class with a virtual destructor suppresses the implicit
    // ones, and left protected because copying an implementation through a
    // reference to this base would slice it.
    AccelerationField() = default;
    AccelerationField(const AccelerationField&) = default;
    AccelerationField(AccelerationField&&) = default;
    AccelerationField& operator=(const AccelerationField&) = default;
    AccelerationField& operator=(AccelerationField&&) = default;
};

} // namespace orrery::integrators
